/*
 * kitty_win.c - the Win32 glue that stays with the terminal window:
 *   - the auto-login password consent,
 *   - inline terminal messages: the session comment and connection errors,
 *   - terminal system-menu actions, the config-box respawn and the
 *     window-title placeholder window,
 *   - the missing-optional-features report and the unverified-agent notice,
 *   - the application-wide theme and check-for-updates settings.
 * The file and folder pickers, printing, the clipboard and process launching
 * are in kitty_winutil.c, the themed boxes in kitty_dlgbox.c, the in-app
 * updater in kitty_updater.c.
 */
#include "kitty_win.h"
#include "kitty_authenticode.h"   /* shared Authenticode trust + CN gate */
#include "kitty_notice.h"          /* near-the-clock warning window */
#include "kitty_rc_additions.h"   /* IDD_UPDATEBOX, IDC_UPD_TEXT, IDC_UPD_UPDATE */
#include "kitty_theme.h"           /* the app-wide colour theme */
#include "../windows/putty-rc.h"   /* -demo-templates: the shared dialog ids */
#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include <wininet.h>   /* CheckVersionFromWebSite: GitHub releases query */
#include <wintrust.h>  /* in-app updater: Authenticode trust verification */
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <msi.h>       /* in-app updater: install-type detection by UpgradeCode */
#include "kitty_gui.h"
#include "kitty.h"
#include "kitty_params.h"
#include "kitty_storage.h"
#include "kitty_auxpos.h"
#include "kitty_winutil.h"   /* debug_logevent */
#include "kitty_dlgbox.h"    /* the themed boxes, kitty_dialog_icon */

/* KiTTY auto-login password consent. Shown the first time the user sets an
 * auto-login password in the configuration dialog (NOT at login time, so the
 * auto-login the user configured is never interrupted). Returns nonzero if the
 * user agrees to store the (reversibly-encrypted) password. */
/* windows/dialog.c: the window a modal raised from the configuration
 * box belongs on. kitty_win.c does not include dialog.h. */
HWND kitty_cfg_modal_owner(void);

int kitty_autopw_warn( void ) {
	int r = kitty_message_box( kitty_cfg_modal_owner(),
		KT_WIN_AUTOPW_WARN,
		KT_CAP_AUTOPW,
		MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) ;
	return (r == IDYES) ;
}

/*
 * Shared renderer for the non-modal connection-error paths in
 * windows/window.c win_seat_connection_fatal / win_seat_nonfatal (cf.
 * upstream cyd01/KiTTY #548): print the error INLINE in the terminal --
 * red "Fatal Error" or yellow "Error" label, default-coloured detail --
 * instead of a modal box that traps the window.  Newlines are normalised
 * to CRLF so the message doesn't staircase down the terminal; for fatal
 * errors a trailing empty quoted description (servers often send
 * '...: ""') is trimmed.  The caller keeps the seat-side consequences
 * (mouse pointer, session close / titlebar marker).
 */
/* KiTTY: the session's Comment, framed, at the clean top of the session -
 * printed BEFORE the connection is started (windows/window.c, just ahead of
 * start_backend), so a note about the session is there at once and is on
 * screen even when the connection never comes up. Only when the Comment
 * panel's "Notify the user at login" is on and there is a note. */
void kitty_print_session_comment(Terminal *term, Conf *conf)
{
    const char *c = conf ? conf_get_str(conf, CONF_comment) : NULL;
    char *body, *line;
    size_t bl = 0;
    const char *p;
    if (!term || !c || !c[0] || !conf_get_bool(conf, CONF_comment_notify))
        return;
    body = snewn(strlen(c) * 2 + 1, char);
    for (p = c; *p; p++) {
        if (*p == '\r') continue;
        else if (*p == '\n') { body[bl++] = '\r'; body[bl++] = '\n'; }
        else body[bl++] = *p;
    }
    body[bl] = 0;
    line = dupprintf(
        KT_WIN_SESSION_NOTE_FRAME, body);
    term_data(term, line, strlen(line));
    sfree(line);
    sfree(body);
}

void kitty_term_print_inline_error(Terminal *term, const char *msg, int fatal)
{
    size_t mlen = msg ? strlen(msg) : 0;
    char *body = snewn(mlen * 2 + 1, char);
    size_t bl = 0;
    const char *p;
    char *line;
    for (p = msg ? msg : ""; *p; p++) {
        if (*p == '\r') continue;
        else if (*p == '\n') { body[bl++] = '\r'; body[bl++] = '\n'; }
        else body[bl++] = *p;
    }
    body[bl] = 0;
    if (fatal && bl >= 2 && body[bl-1] == '"' && body[bl-2] == '"') {
        bl -= 2;
        while (bl > 0 && (body[bl-1] == ' ' || body[bl-1] == ':' ||
                          body[bl-1] == '\r' || body[bl-1] == '\n')) bl--;
        body[bl] = 0;
    }
    line = dupprintf("\r\n\x1b[1;3%cm%s %s:\x1b[0m %s\r\n",
                     fatal ? '1' : '3', appname,
                     fatal ? KT_WIN_FATAL_ERROR : KT_CAP_ERROR, body);
    term_data(term, line, strlen(line));
    sfree(line);
    sfree(body);
}


/* ---- System-menu command handlers (KiTTY) --------------------------------
 * Bodies of a few WM_COMMAND cases in windows/window.c that manipulate only
 * the Win32 window and the session Conf (no window.c statics), lifted here so
 * the WndProc dispatch stays a thin one-line call per case and the upstream
 * file keeps a smaller diff. Cases that touch window.c internals (e.g. the
 * terminal resize path via reset_window) deliberately stay inline there. */


/* IDM_TRANSPARUP / IDM_TRANSPARDOWN: step the layered-window transparency.
 * Refuses on both opt-outs. -1 used to be clamped to 0 and stepped from there,
 * which let the menu undo a setting the keyboard already respected. */
void kitty_menu_adjust_transparency(HWND term_hwnd, Conf *conf, int up)
{
    int t = conf_get_int(conf, CONF_transparencynumber);
    if (!GetTransparencyFlag() || t < 0) return;
    t += up ? 10 : -10;
    if (t < 0) t = 0; if (t > 254) t = 254;
    conf_set_int(conf, CONF_transparencynumber, t);
    {
        void kitty_painter_before_layering(HWND);   /* window.c */
        kitty_painter_before_layering(term_hwnd);
    }
    SetWindowLongPtr(term_hwnd, GWL_EXSTYLE,
        GetWindowLongPtr(term_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(term_hwnd, 0, (BYTE)(255 - t), LWA_ALPHA);
}

/* Position of a DIRECT child of this menu, or -1. Deliberately not
 * GetMenuState/MF_BYCOMMAND: those search submenus, so asked about the system
 * menu they answer about the Window submenu inside it - which meant the edits
 * landed one menu deeper than the position checks around them. */
static int kitty_menu_pos_of(HMENU menu, UINT id)
{
    int n = GetMenuItemCount(menu), i;
    for (i = 0; i < n; i++)
        if (GetMenuItemID(menu, i) == id)
            return i;
    return -1;
}

/* Add or remove the two transparency entries so the menu matches the session.
 * Called for every popup that opens; the anchor item (Font Up) identifies the
 * Window submenu, and being a DIRECT child is the test, so the system menu
 * that merely contains that submenu is left alone. IDs are passed in rather
 * than included, to keep the IDM_ table in one place. */
void kitty_sync_transparency_menu(HMENU menu, Conf *conf, UINT id_up,
                                  UINT id_down, UINT id_anchor)
{
    MENUITEMINFO mii;
    int anchor;
    bool want, have;

    if (!menu)
        return;
    anchor = kitty_menu_pos_of(menu, id_anchor);
    if (anchor < 0)
        return;                         /* not the Window submenu itself */

    want = GetTransparencyFlag() &&
        conf_get_int(conf, CONF_transparencynumber) != -1;
    have = kitty_menu_pos_of(menu, id_up) >= 0;
    if (want == have)
        return;

    if (!want) {
        DeleteMenu(menu, id_up, MF_BYCOMMAND);
        DeleteMenu(menu, id_down, MF_BYCOMMAND);
        /* The pair was followed by a separator, i.e. the item just before the
         * anchor. Found relative to the anchor rather than assumed at the top,
         * so it stays right if the submenu is reordered later. */
        anchor = kitty_menu_pos_of(menu, id_anchor);
        if (anchor > 0) {
            memset(&mii, 0, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (GetMenuItemInfo(menu, anchor - 1, TRUE, &mii) &&
                (mii.fType & MFT_SEPARATOR))
                DeleteMenu(menu, anchor - 1, MF_BYPOSITION);
        }
    } else {
        /* each insert goes before the anchor, which shifts down by one */
        InsertMenu(menu, anchor, MF_BYPOSITION, id_up, KT_SYSMENU_TRANSPARENCY_UP);
        InsertMenu(menu, anchor + 1, MF_BYPOSITION, id_down, KT_SYSMENU_TRANSPARENCY_DOWN);
        InsertMenu(menu, anchor + 2, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    }
}

/* IDM_VISIBLE: toggle always-on-top. */
void kitty_menu_toggle_alwaysontop(HWND term_hwnd, Conf *conf)
{
    bool on = !conf_get_bool(conf, CONF_alwaysontop);
    conf_set_bool(conf, CONF_alwaysontop, on);
    SetWindowPos(term_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    kitty_refresh_title();   /* keep the (ONTOP) title marker in sync */
}

/* [ConfigBox] noexit=yes: launch a fresh instance of ourselves with no
 * arguments, i.e. the configuration box, so closing a session lands the user
 * back in the session picker. Called from WinMain's exit path. */
void kitty_respawn_config_box(void)
{
    char module[MAX_PATH + 1] = "", cmd[MAX_PATH + 3] = "";
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    if (!GetModuleFileName(NULL, module, MAX_PATH)) return;
    snprintf(cmd, sizeof(cmd), "\"%s\"", module);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        /* let the new config box take the foreground despite us being the
         * dying foreground process (same dance as RunCommand) */
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

/* IDM_REPOS: move the window to x,y (clamped to >=1), remembering it in conf. */
void kitty_menu_reposition(HWND term_hwnd, Conf *conf, int x, int y)
{
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    conf_set_int(conf, CONF_xpos, x);
    conf_set_int(conf, CONF_ypos, y);
    SetWindowPos(term_hwnd, 0, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

/* ---- Window-title placeholder reference (config box, Window > Behaviour) ----
 *
 * The placeholders KiTTY expands in a window title. Classic KiTTY listed them
 * as eight static lines squeezed into the panel; this is a modeless window, so
 * the list stays readable WHILE the title is being typed, and each entry can be
 * copied instead of retyped from memory. Keep in step with
 * kitty_expand_wintitle() and docs/window-title-placeholders.md.
 *
 * The codes carry two '%' because that is what goes into the field: a title is
 * run through a printf-style expansion first, so the clipboard has to hand over
 * exactly what must be pasted. */
static const struct { const char *code, *desc; } kitty_title_vars[] = {
    { "%%h", KT_WIN_TITLEVAR_H },
    { "%%s", KT_WIN_TITLEVAR_S },
    { "%%u", KT_WIN_TITLEVAR_U },
    { "%%p", KT_WIN_TITLEVAR_P },
    { "%%P", KT_WIN_TITLEVAR_PROTO },
    { "%%f", KT_WIN_TITLEVAR_F },
    { "%%l", KT_WIN_TITLEVAR_L },
    { "%%d", KT_WIN_TITLEVAR_D },
};

static HWND kitty_titlevars_dlg = NULL;

/* kitty_auxpos.c - shared aux-window placement/memory, as used by the About
 * boxes and the /help window. */

/* Put the selected placeholder - the code alone, not its description - on the
 * clipboard. */
static void kitty_titlevars_copy(HWND hwnd)
{
    int sel = (int)SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_GETCURSEL, 0, 0);
    HGLOBAL h;
    char *p;
    size_t n;
    if (sel < 0 || sel >= (int)lenof(kitty_title_vars)) { MessageBeep(0); return; }
    n = strlen(kitty_title_vars[sel].code) + 1;
    h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!h) return;
    p = (char *)GlobalLock(h);
    if (!p) { GlobalFree(h); return; }
    memcpy(p, kitty_title_vars[sel].code, n);
    GlobalUnlock(h);
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, h);   /* the clipboard owns it now */
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

static INT_PTR CALLBACK TitleVarsProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
      case WM_INITDIALOG: {
        size_t i;
        char line[160];
        for (i = 0; i < lenof(kitty_title_vars); i++) {
            snprintf(line, sizeof(line), "%-6s %s",
                     kitty_title_vars[i].code, kitty_title_vars[i].desc);
            SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_ADDSTRING,
                               0, (LPARAM)line);
        }
        SendDlgItemMessage(hwnd, IDC_TITLEVARS_LIST, LB_SETCURSEL, 0, 0);
        /* The window wears the icon of whatever raised it, like every other
         * KiTTY window - it had none, which also left a blank in the
         * taskbar. */
        kitty_dialog_icon(hwnd, NULL);
        kitty_auxpos_apply(hwnd, "TitleVars", GetWindow(hwnd, GW_OWNER), 1);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_TITLEVARS_COPY:
            kitty_titlevars_copy(hwnd);
            return 1;
          case IDC_TITLEVARS_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) { kitty_titlevars_copy(hwnd); return 1; }
            return 0;
          case IDOK: case IDCANCEL:
            DestroyWindow(hwnd);
            return 1;
        }
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 1;
      case WM_DESTROY:
        kitty_auxpos_save(hwnd, "TitleVars");
        ShinyRemoveAuxDialog(hwnd);
        kitty_titlevars_dlg = NULL;
        return 0;
    }
    return 0;
}

/* Open (or re-focus) the placeholder list. Modeless, and registered as an aux
 * dialog so it keeps its keyboard handling while the modal configuration box is
 * up - the whole point being that it can sit beside the field being edited. */
void kitty_show_title_placeholders(HWND owner)
{
    if (kitty_titlevars_dlg && IsWindow(kitty_titlevars_dlg)) {
        SetForegroundWindow(kitty_titlevars_dlg);
        return;
    }
    kitty_titlevars_dlg = CreateDialog(hinst, MAKEINTRESOURCE(IDD_TITLEVARS),
                                       owner, TitleVarsProc);
    if (kitty_titlevars_dlg) {
        ShinyAddAuxDialog(kitty_titlevars_dlg);
        ShowWindow(kitty_titlevars_dlg, SW_SHOW);
        SetForegroundWindow(kitty_titlevars_dlg);
    }
}

/* IDM_HYPERLINKTOGGLE: flip runtime URL detection and sync the menu check. */
void kitty_menu_toggle_hyperlink(HWND hwnd)
{
    int GetHyperlinkFlag(void);
    void SetHyperlinkFlag(int flag);
    int nf = !GetHyperlinkFlag();
    SetHyperlinkFlag(nf);
    CheckMenuItem(GetSystemMenu(hwnd, FALSE), IDM_HYPERLINKTOGGLE,
                  MF_BYCOMMAND | (nf ? MF_CHECKED : MF_UNCHECKED));
}


/* ------------------------------------------------------------------ *
 * KiTTY: client-side serving-agent verification (security pass #3).
 *
 * When kitty.exe asks the SSH agent to list or sign, SOMETHING answers on
 * our agent pipe / Pageant window. This confirms that something is a
 * genuine, our-publisher-signed KiTTY/kageant, and warns once if not - a
 * hostile program that grabbed the pipe/window would otherwise see every
 * key operation this session performs.
 *
 * Fail-quiet and best-effort: only a SIGNED (release) kitty.exe can
 * honestly demand a signed agent, so an unsigned dev build says nothing;
 * an unreadable server process says nothing. Never blocks the query - the
 * answer is already in hand when this runs. Configurable off via
 * [KiTTY] verifyagent=no.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * What this Windows could not do.
 *
 * Every optional API KiTTY resolves at runtime is recorded with the feature
 * it powers (kitty_oldwin.c). On a Windows old enough to lack some of them
 * those features simply never happen - and "it did nothing" is the worst way
 * to learn that dark mode, or Windows Hello, is missing because of the
 * operating system rather than because of a setting.
 *
 * So it is said twice: in full in the Event Log, where someone diagnosing
 * looks, and as ONE line in the terminal, where someone who is not
 * diagnosing will actually see it. The line is switched off with
 * [KiTTY] warnmissingfeatures=no - a fact about the machine does not change,
 * so whoever has read it once can stop being told.
 *
 * Once per process, which is once per terminal window, and never again on a
 * reconnect - unlike start_backend, which runs for every reconnect too.
 * ------------------------------------------------------------------ */
void kitty_report_missing_features(Terminal *term)
{
    static int done = 0;
    char *full, *brief, cfg[16];

    if (done)
        return;
    done = 1;

    full = kitty_oldwin_degraded();
    if (!full)
        return;                        /* this Windows has everything */

    /* The Event Log gets it whole, and gets it whatever the setting says:
     * switching the notice off is about not being interrupted, not about
     * hiding the answer from whoever goes looking for it. */
    debug_logevent("%s", full);
    sfree(full);

    if (ReadParameter(KI_SECTION_KITTY, KI_WARNMISSINGFEATURES, cfg) &&
        !stricmp(cfg, "no"))
        return;

    brief = kitty_oldwin_degraded_brief();
    if (brief && term) {
        /* Yellow "NOTE:" with the body in the terminal's own colours - the
         * shape of the post-quantum advisory, one step below its red, because
         * this is information rather than a warning. */
        char *line = dupprintf(
            KT_WIN_MISSING_FEATURES_LINE, brief);
        term_data(term, line, strlen(line));
        sfree(line);
    }
    sfree(brief);
}

/*
 * The agent check's work, on a worker thread: two signature verifications
 * (this binary, then the program serving the agent pipe), each a
 * WinVerifyTrust call. They used to run inside the first agent query, on the
 * UI thread, with an online revocation check, so a machine without a route
 * to the CRL servers sat in that call for seconds before its first key
 * login could continue. Verification is cache-only here
 * (kitty_authenticode_verify_offline): the publisher-CN pin is the gate for
 * this notice, and a revoked certificate of ours is a release matter, not
 * something a client's agent check can settle. The verdict travels back as
 * WM_KITTY_AGENT_CHECKED; the notice itself is shown by the UI thread.
 */
struct agent_check_job { unsigned long server_pid; };

static DWORD WINAPI kitty_agent_check_thread(LPVOID arg)
{
    struct agent_check_job *job = (struct agent_check_job *)arg;
    unsigned long pid = job->server_pid;
    char self[MAX_PATH], srv[MAX_PATH];
    HANDLE h;
    int gotpath;
    HWND GetMainHwnd(void);

    sfree(job);
    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0)
        return 0;
    /* An unsigned build cannot honestly insist the agent be signed. */
    if (!kitty_authenticode_verify_offline(self))
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        return 0;                      /* cannot inspect: stay quiet */
    gotpath = kitty_process_image_path(h, srv, sizeof(srv));
    CloseHandle(h);
    if (!gotpath)
        return 0;
    if (kitty_authenticode_verify_offline(srv))
        return 0;                      /* genuine KiTTY/kageant - all good */
    {
        const char *base = strrchr(srv, '\\');
        char *name = dupstr(base ? base + 1 : srv);
        HWND main = GetMainHwnd();
        if (!main || !PostMessage(main, WM_KITTY_AGENT_CHECKED, 0, (LPARAM)name))
            sfree(name);
    }
    return 0;
}

/* WM_KITTY_AGENT_CHECKED, on the UI thread. `name` is the serving program's
 * file name from the worker (freed here). */
void kitty_agent_unverified_notice(char *name)
{
    /* Say WHO is speaking (this terminal, not the agent) before saying
     * what was found - an anonymous amber box reads as "something says
     * my key is compromised" and confuses more than it warns. And say
     * what is actually at stake: the program SERVES the keys, so it can
     * see and sign with them - that is not the same as "your key
     * material leaked", which the first wording implied. Clicking the
     * notice lands on the setting that turns the warning off, for
     * people who run another agent on purpose. */
    HWND GetMainHwnd(void);
    char *msg = dupprintf(KT_WIN_AGENT_UNVERIFIED, name);
    kitty_notice_show(KT_CAP_AGENT_UNVERIFIED, msg,
                      RGB(190, 110, 0), 15,
                      GetMainHwnd(), WM_KITTY_AGENT_UNVERIFIED);
    sfree(msg);
    sfree(name);
}

static void kitty_agent_serving_check(unsigned long server_pid, int transport)
{
    static int done = 0;
    char cfg[16];
    struct agent_check_job *job;
    HANDLE t;
    (void)transport;

    if (done)
        return;

    /* Opt-out. */
    if (ReadParameter(KI_SECTION_KITTY, KI_VERIFYAGENT, cfg) && !stricmp(cfg, "no")) {
        done = 1;
        return;
    }

    if (server_pid == 0)
        return;                        /* unknown this time; retry later */

    done = 1;
    job = snew(struct agent_check_job);
    job->server_pid = server_pid;
    t = CreateThread(NULL, 0, kitty_agent_check_thread, job, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        sfree(job);
}

void kitty_install_agent_check(void)
{
    agent_serving_check_hook = kitty_agent_serving_check;
}

/*
 * KiTTY: the application-wide colour theme, [KiTTY] theme in kitty.ini and the
 * value of the same name in the registry. It is read through ReadParameterN,
 * which is what makes portable mode, the session hive and the read-only flag
 * apply to it exactly as they do to every other global setting; the satellite
 * binaries reach the same value through kitty/kitty_theme_pref.c.
 *
 * Handed to the theme module at startup, so it is asked once per dialog rather
 * than frozen at whatever it meant when KiTTY started - which is what lets
 * "follow the system" change with the system while a window is open.
 */
int ReadParameterN(const char *key, const char *name,
                   char *value, size_t size);   /* kitty.c */
#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif

/* Remembered for two seconds: the theme hook asks on every activation of
 * every dialog-class window, which is a store read (and a stat of kitty.ini)
 * per click-to-focus. The Application panel's setter forgets it at once
 * (kitty_theme_app_pref_forget), so a change applies to the next dialog. */
static int kitty_theme_pref_cached = -1;
static DWORD kitty_theme_pref_stamp = 0;

void kitty_theme_app_pref_forget(void)
{
    kitty_theme_pref_cached = -1;
}

int kitty_theme_app_pref(void)
{
    char buf[32];
    int v;
    DWORD now = GetTickCount();
    if (kitty_theme_pref_cached >= 0 && now - kitty_theme_pref_stamp < 2000)
        return kitty_theme_pref_cached;
    buf[0] = '\0';
    v = KITTY_THEME_SYSTEM;
    if (ReadParameterN(INIT_SECTION, KI_THEME, buf, sizeof(buf))) {
        v = kitty_theme_pref_from_string(buf);
        if (v < 0)
            v = KITTY_THEME_SYSTEM;
    }
    kitty_theme_pref_cached = v;
    kitty_theme_pref_stamp = now;
    return v;
}

bool kitty_theme_app_dark(void)
{
    return kitty_theme_dark_for(kitty_theme_app_pref());
}

/*
 * Whether to look for a new release at startup - an APPLICATION setting, in
 * kitty.ini as [KiTTY] checkupdate, defaulting to ON.
 *
 * It used to be CONF_check_update_startup, stored in every saved session and
 * read from whichever session opened first, which meant the answer depended on
 * which host you connected to. The old per-session key is retired on save (see
 * kitty_retired_keys in windows/storage.c), so it drains out of the store
 * rather than being migrated: there is nothing to migrate, since the setting
 * was never per-session in meaning.
 *
 * Read through ReadParameterN like the theme, so it works the same in every
 * save mode and can be answered without a Conf in hand.
 */

int kitty_check_update_enabled(void)
{
    char buf[32];
    buf[0] = '\0';
    if (!ReadParameterN(INIT_SECTION, KI_CHECKUPDATE, buf, sizeof(buf)))
        return 1;                      /* not set: on */
    return !(!_stricmp(buf, "no") || !_stricmp(buf, "0") ||
             !_stricmp(buf, "false") || !_stricmp(buf, "off"));
}

void kitty_set_check_update_enabled(int on)
{
    WriteParameter(INIT_SECTION, KI_CHECKUPDATE, on ? "yes" : "no");
}
