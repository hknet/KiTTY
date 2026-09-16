/*
 * kitty_config.c - the configuration box's sequencer. setup_config_box builds the
 * panels in tree order (the dialog asserts on any other order) and the three
 * dispatchers route the per-panel hooks of windows/dialog.c to whichever
 * panel owns the path. The panels themselves live in kitty_config_session.c
 * (Session, Terminal, Window, Connection), kitty_config_app.c (the
 * Application tab) and kitty_config_upstream.c (PuTTY's own handlers);
 * kitty_config_shared.c holds what both tabs use. This set replaces PuTTY's
 * config.c: it is compiled with MOD_PERSO into the kitty and kitty_portable
 * targets only and wins at link time over the library's config.o.
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

/* The workplace controls of the config box that is open, so the poll below can
 * find them. One config box at a time; cleared when its panel is rebuilt. */
struct wpmode_data *kitty_wpmode_active = NULL;
struct iniview_data *kitty_iniview_active = NULL;
struct migf_data *kitty_migf_active = NULL;

/* KiTTY: Security > Host keys - the trust store listed (kitty_hostkeys.c);
 * the fill hook grows its list, so the state is declared up here. */
struct kitty_hostkey_list;
struct kitty_hkv_run;
struct hk_verdict;
struct hk_data *kitty_hk_active = NULL;

/*
 * Which panel the config box should open on, instead of Session.
 *
 * Set by "kitty.exe -cfgpanel Connection/Proxy" (windows/window.c parses it),
 * which is how the "your workplace proxy did not answer" notice puts the user
 * in front of the switch rather than switching anything off for them: a single
 * stray click should not tear down a proxy setup they may still want.
 */
static char kitty_cfgbox_panel[128] = "";

void kitty_cfgbox_open_on_panel(const char *path)
{
    if (!path || !path[0] || strlen(path) >= sizeof(kitty_cfgbox_panel))
        kitty_cfgbox_panel[0] = '\0';
    else
        strcpy(kitty_cfgbox_panel, path);
}

const char *kitty_cfgbox_wanted_panel(void)
{
    return kitty_cfgbox_panel[0] ? kitty_cfgbox_panel : NULL;
}

/*
 * Whether the session this box restores at startup counts as DELIBERATELY
 * loaded ("-cfgloaded", set beside -cfgpanel by the launcher's
 * hotkey-conflict balloon, which names a session and opens it for fixing).
 *
 * The distinction matters to the overwrite guard: an ordinary fresh box
 * restores the last session merely for convenience, so saving over it still
 * warns - typing a new session's settings into a fresh box must not
 * silently destroy the restored one. A balloon click IS a load in the
 * user's mind: warning about the very session it named makes the fix it
 * asked for look destructive.
 */
int kitty_cfgbox_loaded_deliberate = 0;

void kitty_cfgbox_open_loaded(void)
{
    kitty_cfgbox_loaded_deliberate = 1;
}
/*
 * The per-panel hooks windows/dialog.c calls: which control a panel lets
 * grow with the box (fill), what to do once the panel is laid out (placed),
 * and what to do when it is shown or hidden (shown). One row per panel that
 * needs any of them; a panel not listed needs none, and a new panel is one
 * row here rather than three if-chains. The footer pin applies to every
 * panel and stays outside the table.
 */
struct kitty_panel_hook {
    const char *path;
    dlgcontrol *(*fill)(void);
    void (*placed)(void);
    void (*shown)(bool show);
};

static dlgcontrol *hook_session_fill(void)
{
    return kitty_session_ssd ? kitty_session_ssd->listbox : NULL;
}
static void hook_session_placed(void) { kitty_config_session_distribute(); }

/* The kitty.ini view: a file is as long as it is, so the box gets the
 * height the window has to give. */
static dlgcontrol *hook_iniview_fill(void)
{
    return (kitty_iniview_active && kitty_iniview_active->view) ?
        kitty_iniview_active->view : NULL;
}
static void hook_iniview_placed(void) { kitty_iniview_place(); }

/* The folder-import list: a scan may find hundreds of files. */
static dlgcontrol *hook_migf_fill(void)
{
    return (kitty_migf_active && kitty_migf_active->listbox) ?
        kitty_migf_active->listbox : NULL;
}

/* The host-key list, likewise; its splitter bar is not one of the panel's
 * controls, so the panel cache does not hide it with them - it has to go and
 * come by itself, or it lies across the next panel (artefacts on whatever
 * panel followed Host keys). Same SWP_NOREDRAW discipline as the controls. */
static dlgcontrol *hook_hk_fill(void)
{
    return (kitty_hk_active && kitty_hk_active->listbox) ?
        kitty_hk_active->listbox : NULL;
}
static void hook_hk_placed(void) { hk_place_splitter(kitty_hk_active); }
static void hook_hk_shown(bool show)
{
    if (!hk_splitter || !IsWindow(hk_splitter))
        return;
    SetWindowPos(hk_splitter, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW |
                 (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

/* Security > Applications: one row per program file, the taller the better. */
static dlgcontrol *hook_apps_fill(void) { return kitty_apps_fill_ctrl(); }

/* The shortcut editor's lists: one row per action, one per AutoText entry,
 * the taller the better. */
static dlgcontrol *hook_sc_fill(void) { return kitty_sc_fill_ctrl(false); }
static dlgcontrol *hook_sc_autotext_fill(void) { return kitty_sc_fill_ctrl(true); }

static const struct kitty_panel_hook kitty_panel_hooks[] = {
    { "Session",              hook_session_fill,     hook_session_placed, NULL },
    { KCFG_PATH_INIVIEW,      hook_iniview_fill,     hook_iniview_placed, NULL },
    { KCFG_PATH_OLD_FOLDERS,  hook_migf_fill,        NULL,                NULL },
    { KCFG_PATH_HOSTKEYS,     hook_hk_fill,          hook_hk_placed,      hook_hk_shown },
    { KCFG_PATH_APPLICATIONS, hook_apps_fill,        NULL,                NULL },
    { KCFG_PATH_SHORTCUTS,    hook_sc_fill,          NULL,                NULL },
    { KCFG_PATH_AUTOTEXT,     hook_sc_autotext_fill, NULL,                NULL },
};

static const struct kitty_panel_hook *kitty_panel_hook_for(const char *path)
{
    size_t i;
    if (!path)
        return NULL;
    for (i = 0; i < lenof(kitty_panel_hooks); i++)
        if (!strcmp(kitty_panel_hooks[i].path, path))
            return &kitty_panel_hooks[i];
    return NULL;
}

dlgcontrol *kitty_config_panel_fill_ctrl(const char *path)
{
    const struct kitty_panel_hook *h = kitty_panel_hook_for(path);
    return (h && h->fill) ? h->fill() : NULL;
}

/* windows/dialog.c calls this once per panel, as soon as it is laid out. */
void kitty_config_panel_placed(const char *path)
{
    const struct kitty_panel_hook *h = kitty_panel_hook_for(path);
    if (h && h->placed)
        h->placed();
    if (path)
        kitty_config_footer_pin(path);  /* the app panels' footer, likewise */
}

/* A panel switch. */
void kitty_config_panel_shown(const char *path, bool show)
{
    const struct kitty_panel_hook *h = kitty_panel_hook_for(path);
    if (h && h->shown)
        h->shown(show);
}

void setup_config_box(struct controlbox *b, bool midsession,
                      int protocol, int protcfginfo)
{
    scb_panel_session(b, midsession);
    /* Comment FIRST under Session, so a note about the session is the first
     * thing its subtree offers. */
    scb_panel_comment(b);
    scb_panel_logging(b, midsession, protocol);
    scb_panel_scripting(b, midsession);
    scb_panel_terminal(b);
    scb_panel_window(b, midsession, protocol);
    scb_panel_selection(b);
    scb_panel_connection(b, midsession, protocol);
    scb_panel_ssh(b, midsession, protocol, protcfginfo);
    scb_panel_serial(b, midsession, protocol);
    /* Proxy just before ZModem; the rare protocols close the subtree. */
    scb_panel_proxy(b, midsession);
    scb_panel_transfers(b);
    scb_panel_zmodem(b);
    scb_panel_other_protocols(b, midsession, protocol);
    /* LAST: everything above is the Session tab, and the tree build splits the
     * two on the "Application/" prefix. Keeping them contiguous means the
     * split is a prefix test rather than a lookup. */
    scb_panel_application(b, midsession);
}
