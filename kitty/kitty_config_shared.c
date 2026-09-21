/*
 * kitty_config_shared.c - what both tabs of the configuration box use: the INT-typed
 * checkbox handler, the dialog helpers, the kitty.ini settings engine (the
 * kset table and its handler, which Application leaves and two session
 * panels mount), the remembered category folds, the box size and the
 * red/bold caption rules.
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
#include "kitty_winutil.h"
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
#include "kitty_params.h"
#include "kitty_broadcast.h"
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

/* Declarations from other KiTTY modules that this file does not reach
 * through a header. The MOD_PERSO fences in this file are always true: it
 * is never compiled without that define. */
/* KiTTY folder-management engine (kitty_config.c does not include kitty_tools.h/kitty.h) */
/* Selects an editable combo's whole text so the next keystroke replaces it;
 * see the implementation comment in windows/controls.c for why the field is
 * not simply emptied instead. */

#define KITTY_LAUNCHER_REFRESH_MESSAGE "KiTTYLauncherRefreshSessionsAndHotkeys"

/* ==== Helpers both tabs use ============================================= */

void kitty_notify_launcher_sessions_changed(void)
{
    UINT msg = RegisterWindowMessageA(KITTY_LAUNCHER_REFRESH_MESSAGE);
    if (msg)
        PostMessageA(HWND_BROADCAST, msg, 0, 0);
}

/* Checkbox handler for KiTTY keys that are stored as INT (0/1) rather
 * than BOOL (the standard conf_checkbox_handler asserts on INT keys in
 * 0.84). Context is the CONF_ key. */
void kitty_checkbox_int_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH)
        dlg_checkbox_set(ctrl, dlg, conf_get_int(conf, key) != 0);
    else if (event == EVENT_VALCHANGE)
        conf_set_int(conf, key, dlg_checkbox_get(ctrl, dlg) ? 1 : 0);
}

/*
 * The control's caption, as a FUNCTION of (choice, session) rather than a pair of
 * hard-coded strings - workplace-proxy mode adds a third state to this same
 * control, and hard-coding two would mean rewriting it then.
 *
 * An override is active whenever the choice would change what the STORED session
 * does. That includes picking "No proxy" for a session that has one: suppressing
 * the session's proxy is a real change, not a neutral position.
 */
/* The two captions, named so that windows/dialog.c can recognise the active one
 * and draw it BOLD without knowing anything else about this control. */

/* The line-spacing label. Unlike the captions above, this one does not change
 * its wording when it turns red - a static is laid out once at the width of its
 * initial text, so a longer string is simply cut off. The colour is driven by
 * the flag below instead, and the text is only re-set to force a repaint. */
bool g_linespc_out_of_range = false;

/* Called from windows/dialog.c's WM_CTLCOLORSTATIC for every static in the config
 * box, so it must be cheap and must answer false for everything else. Stubbed to
 * false for the stock variants in windows/kitty_config_stubs.c. */
bool kitty_red_caption(const char *text)
{
    if (!text)
        return false;
    if (!strcmp(text, KITTY_PROXY_LABEL_ACTIVE))
        return true;
    /* Workplace proxy mode's live state line while the mode is ON. Prefix, not
     * equality: the proxy's name is appended to it. Same bold red as above and
     * for the same reason - something is overriding this session right now, and
     * in ordinary body text that sentence was read as more of the paragraph
     * around it. */
    if (!strncmp(text, "Workplace proxy mode is ON",
                 strlen("Workplace proxy mode is ON")))
        return true;
    /* The line-spacing label while the typed number is outside 100-300. The
     * value is clamped when the fonts are built either way, so the red is saying
     * that the number in the box is not the number in use. */
    return g_linespc_out_of_range && !strcmp(text, KITTY_LINESPC_LABEL);
}

/* Captions drawn BOLD (but in the ordinary colour, unlike the one above): the
 * lead line of the workplace-proxy box, because that box is the one thing on
 * the Proxy panel that is NOT part of the session in front of you, and it has
 * to look different at a glance rather than on a careful read. Matched by text
 * for the same reason as above - no id has to be plumbed through the portable
 * control layer. Same cheap-and-false-by-default contract, and the same stub in
 * windows/kitty_config_stubs.c.
 *
 * WARNING: The box's GROUP TITLE cannot be bolded this way: a
 * group box is a themed BUTTON and draws its own caption, ignoring the font
 * selected into the DC here. Hence the bold lead line INSIDE the box - which is
 * an ordinary static, and does honour it. */
/* One sentence, used wherever a control sits on a session's panel without
 * belonging to the session - the workplace-proxy box, and the WinSCP executable
 * path further down. Saying it the same way every time is the point. */
#define KITTY_NOT_SESSION_LEAD    "This is NOT a setting of this session."

bool kitty_bold_caption(const char *text)
{
    return text && !strcmp(text, KITTY_NOT_SESSION_LEAD);
}

/*
 * Connection/Proxy: switching WORKPLACE PROXY MODE on and off.
 *
 * The mode routes EVERY connection this install starts through one chosen
 * proxy, whatever each session stores, until it is switched off. It is not a
 * session setting and nothing here is saved into the session: what is written
 * is the SELECTION (which proxy the mode uses), because the mode being ON is
 * only ever a launcher holding the arming.
 *
 * So this button does not set a value, it asks the launcher: switch on, and if
 * no launcher of this install is running, one is started already armed. That
 * indirection is the design - the launcher going away is what switches the mode
 * off, so it has to be the thing holding it.
 */
/* Declared again here (and again below, for the WinSCP path): this file keeps
 * its kitty.c accessors next to the code that uses them rather than in a
 * header, and the workplace handler sits above the other copy. */

/* The live state line. The ON wording is matched by windows/dialog.c and drawn
 * BOLD RED - the same treatment the armed proxy-override caption gets, for the
 * same reason: something is overriding this session right now. Prefix-matched,
 * because the proxy's name is appended to it. */
/* Both must fit ONE line at the panel width: the control keeps the size it was
 * given when the panel was built, so a longer replacement is clipped. */

/*
 * KiTTY: let a droplist's DROPPED-DOWN list be wider than the closed control,
 * so entries are readable in full when it is open even though the closed box is
 * only as wide as the panel allows. CB_SETDROPPEDWIDTH is the only way to say
 * this; the portable dlg_* API has no notion of it.
 *
 * Measured from the entries themselves rather than guessed, and clamped to the
 * dialog's own width so it cannot spill off the window.
 */
void kitty_dlg_droplist_fit(dlgcontrol *ctrl, dlgparam *dlg)
{
    int i;
    if (!ctrl || !dlg)
        return;
    for (i = 0; i < dlg->nctrltrees; i++) {
        struct winctrl *c = winctrl_findbyctrl(dlg->controltrees[i], ctrl);
        HWND cb;
        if (!c)
            continue;
        cb = GetDlgItem(dlg->hwnd, c->base_id + 1);   /* label, then the combo */
        if (!cb)
            return;
        {
            HDC dc = GetDC(cb);
            HFONT f = (HFONT)SendMessage(cb, WM_GETFONT, 0, 0);
            HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;
            int n = (int)SendMessage(cb, CB_GETCOUNT, 0, 0), k, wmax = 0;
            RECT dr;
            for (k = 0; k < n; k++) {
                char buf[512];
                SIZE sz;
                int len = (int)SendMessage(cb, CB_GETLBTEXTLEN, k, 0);
                if (len <= 0 || len >= (int)sizeof(buf))
                    continue;
                SendMessageA(cb, CB_GETLBTEXT, k, (LPARAM)buf);
                if (GetTextExtentPoint32A(dc, buf, (int)strlen(buf), &sz) &&
                    sz.cx > wmax)
                    wmax = sz.cx;
            }
            if (old) SelectObject(dc, old);
            ReleaseDC(cb, dc);
            wmax += GetSystemMetrics(SM_CXVSCROLL) + 16;   /* scrollbar + padding */
            if (GetClientRect(dlg->hwnd, &dr) && wmax > dr.right - dr.left - 24)
                wmax = dr.right - dr.left - 24;
            if (wmax > 0)
                SendMessage(cb, CB_SETDROPPEDWIDTH, (WPARAM)wmax, 0);
        }
        return;
    }
}

/* Is this control laid out right now - i.e. is its panel the one on screen?
 * dlg_label_change asserts on a control that is not, so anything refreshing
 * from OUTSIDE a user action (the poll below) has to ask first. Same
 * winctrl_findbyctrl walk as kitty_dlg_enable_button, and here for the same
 * reason: windows/controls.c is built without MOD_PERSO. */
static bool kitty_dlg_ctrl_present(dlgcontrol *ctrl, dlgparam *dlg)
{
    int i;
    if (!ctrl || !dlg)
        return false;
    for (i = 0; i < dlg->nctrltrees; i++)
        if (winctrl_findbyctrl(dlg->controltrees[i], ctrl))
            return true;
    return false;
}

/*
 * Called on a timer by the config box (windows/dialog.c) so that switching the
 * mode from the TRAY reaches a config box that is already open. Without it the
 * box kept saying "ON" until the user left the panel and came back - the state
 * is held by another process, so nothing in this one hears about the change
 *.
 *
 * Deliberately narrow: it repaints the two workplace controls and only when the
 * armed state has actually MOVED. A blanket dlg_refresh() every second would
 * re-read the whole panel from the Conf and could throw away what the user is
 * in the middle of typing in the fields above.
 */
static void kset_deferred_tick(void);   /* the held-back write, below */

void kitty_cfgbox_workplace_poll(dlgparam *dlg)
{
    static int last = -1, last_have = -1;
    char armed[256];
    int now, have;
    struct wpmode_data *wd = kitty_wpmode_active;
    kitty_iniview_poll(dlg);           /* the kitty.ini view follows its file */
    /* A field that writes only once the typing has stopped (the Application
     * Notification) is written from here: this tick is the box's clock. */
    kset_deferred_tick();
    /* The configuration box is the first window of a KiTTY++ started with no
     * session, and of "-cfgbox", so it owes the application notification the
     * same way the terminal and the launcher do. This poll is armed for every
     * box (windows/dialog.c, one second), which is the earliest KiTTY-side
     * tick after the box exists; the flag settles itself, so a box opened
     * from a terminal that already showed the note shows nothing. */
    kitty_notes_show_pending(NULL);
    now = kitty_workplace_query(armed, sizeof(armed)) ? 1 : 0;
    /* Opening the config box is one of the ways KiTTY gets started, so it is
     * also one of the places that owes the "the mode is not active any more"
     * notice when the launcher went away without saying so. Self-clearing:
     * it fires at most once, whichever path reaches it first. Asked only
     * when the mode is NOT held and that is news (the first tick, or the
     * holder just went away): while it is held nothing can be owed, and
     * asking every second meant a registry read per second for the life
     * of the box. */
    if (now == 0 && last != 0)
        kitty_workplace_show_pending_notice();
    if (!wd || !dlg) {
        last = now;
        return;
    }
    have = kitty_has_proxy_definitions() ? 1 : 0;
    if (now == last && have == last_have)
        return;
    last = now;
    last_have = have;
    if (!kitty_dlg_ctrl_present(wd->button, dlg))
        return;                         /* another panel is showing */
    kitty_wpmode_button_label(wd->button, dlg);
    kitty_wpmode_state_label(wd, dlg);
    kitty_wpmode_grey(wd, dlg);
    /* The droplist carries state too - it marks the proxy the mode is using
     * "(in use)" - so it has to follow a change made from the tray as well, or
     * it would go on pointing at a proxy that is no longer in use. */
    if (wd->list)
        dlg_refresh(wd->list, dlg);
}

/*
 * KiTTY: enable or grey out a config-box BUTTON.
 *
 * The cross-platform dlg_* API has no such call and never had one - it only
 * ever carried what every backend could implement, so upstream's own config.c
 * cannot grey a control either. This lives here rather than in dialog.h or
 * windows/controls.c on purpose: both of those rebase with every PuTTY bump,
 * and windows/controls.c compiles into the shared library WITHOUT MOD_PERSO,
 * where a guarded addition would be silently dead code.
 *
 * Buttons only. A button's window id is its base_id; other control types split
 * into several ids and would each need their own rule, which nothing wants yet.
 */
void kitty_dlg_enable_button(dlgcontrol *ctrl, dlgparam *dlg,
                                    bool enabled)
{
    int i;
    if (!ctrl || !dlg)
        return;
    for (i = 0; i < dlg->nctrltrees; i++) {
        struct winctrl *c = winctrl_findbyctrl(dlg->controltrees[i], ctrl);
        if (c) {
            HWND h = GetDlgItem(dlg->hwnd, c->base_id);
            if (h)
                EnableWindow(h, enabled);
            return;
        }
    }
}

/* [ConfigBox] fixedsizewindow: the box keeps its size. dialog.c reads the
 * flag through kitty_cfgbox_size_locked() (it compiles into the stock
 * variants too, where a stub answers 0) and re-fits the frame when the box
 * is ticked, so the change shows in the window it was made in. */
int kitty_cfgbox_size_locked(void)
{
    return GetConfigBoxFixedSizeFlag() != 0;
}

/*
 * A number in kitty.ini, edited as text.
 *
 * It shows the value IN FORCE, not the file's text: both keys have a working
 * default, and an empty box next to a box that plainly has a height reads as
 * "nothing is set here" when something is. The one number that is still shown
 * blank is windowheight = 0, which is not a height at all - it means "however
 * tall the list makes it".
 *
 * An empty box therefore writes nothing rather than writing a 0 that would be
 * read back as a real height.
 */
int cfgwin_refreshing = 0;

/*
 * The box was dragged to a new size: store it as the same two keys the fields
 * below edit, so the drag and the fields are one setting. Called from
 * windows/dialog.c on WM_EXITSIZEMOVE (stubbed out for the stock variants,
 * whose box does not resize).
 *
 * Both the file and the running program, for the reason spelt out in the
 * VALCHANGE arm below: EVENT_REFRESH answers from the running values, so
 * writing only the file leaves the fields showing the old size.
 */
/* ---- remembered category folds --------------------------------------------
 *
 * categoryexpand says how the tree opens by DEFAULT; what the user changed
 * BY HAND wins over it in BOTH directions - a collapsed node stays collapsed
 * and an explicitly expanded one stays expanded, whatever the default says.
 * Only the DEVIATIONS are stored, so changing categoryexpand still moves
 * every node the user never touched.
 *
 * Stored as [ConfigBox] collapsed, a comma-separated list of tree PATHS
 * ("Connection/Login"): a bare path is a collapse override, a path prefixed
 * with '+' an expand override. (Values written before the expand side
 * existed were bare collapse paths and read unchanged.)
 */
#define CFGTREE_FOLDS_MAX 48
static char *cfgtree_folds[CFGTREE_FOLDS_MAX];   /* "path" or "+path" */
static int cfgtree_nfolds = 0;
static int cfgtree_folds_loaded = 0;

static int cfgtree_fold_find(const char *path)
{
    for (int i = 0; i < cfgtree_nfolds; i++) {
        const char *e = cfgtree_folds[i];
        if (*e == '+') e++;
        if (!strcmp(e, path))
            return i;
    }
    return -1;
}

static void cfgtree_folds_load(void)
{
    char buf[2048] = "";
    if (cfgtree_folds_loaded)
        return;
    cfgtree_folds_loaded = 1;
    if (!ReadParameterN(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_COLLAPSED, buf, sizeof(buf)))
        return;
    for (char *p = buf; *p; ) {
        char *q = strchr(p, ',');
        if (q) *q = '\0';
        while (*p == ' ') p++;
        if (*p && cfgtree_nfolds < CFGTREE_FOLDS_MAX &&
            cfgtree_fold_find(*p == '+' ? p + 1 : p) < 0)
            cfgtree_folds[cfgtree_nfolds++] = dupstr(p);
        if (!q) break;
        p = q + 1;
    }
}

/* -1 = no override recorded, 0 = keep it collapsed, 1 = keep it expanded. */
int kitty_cfgtree_get_fold(const char *path)
{
    int i;
    cfgtree_folds_load();
    i = cfgtree_fold_find(path);
    if (i < 0)
        return -1;
    return cfgtree_folds[i][0] == '+' ? 1 : 0;
}

/* Record what the user's tree shows: a state matching the DEFAULT clears any
 * override, a deviation stores one in its direction. */
void kitty_cfgtree_set_fold(const char *path, int expanded,
                            int default_expanded)
{
    int i;
    cfgtree_folds_load();
    i = cfgtree_fold_find(path);
    if (!!expanded == !!default_expanded) {
        if (i >= 0) {
            sfree(cfgtree_folds[i]);
            cfgtree_folds[i] = cfgtree_folds[--cfgtree_nfolds];
        }
        return;
    }
    if (i >= 0) {
        sfree(cfgtree_folds[i]);
        cfgtree_folds[i] = expanded ? dupcat("+", path) : dupstr(path);
    } else if (cfgtree_nfolds < CFGTREE_FOLDS_MAX) {
        cfgtree_folds[cfgtree_nfolds++] =
            expanded ? dupcat("+", path) : dupstr(path);
    }
}

void kitty_cfgtree_folds_save(void)
{
    char buf[2048] = "";
    size_t used = 0;
    if (!cfgtree_folds_loaded)
        return;                        /* nothing was ever read or changed */
    for (int i = 0; i < cfgtree_nfolds; i++) {
        size_t n = strlen(cfgtree_folds[i]);
        if (used + n + 2 >= sizeof(buf)) break;
        if (used) buf[used++] = ',';
        memcpy(buf + used, cfgtree_folds[i], n + 1);
        used += n;
    }
    WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_COLLAPSED, buf);
}

void kitty_cfgbox_store_size(int w, int h)
{
    char buf[32];

    if (w > 0) {
        sprintf(buf, "%d", w);
        WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_WINDOWWIDTH, buf);
        SetConfigBoxWindowWidth(w);
    }
    if (h > 0) {
        sprintf(buf, "%d", h);
        WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_WINDOWHEIGHT, buf);
        SetConfigBoxWindowHeight(h);
    }
}

/* ==== The kitty.ini settings engine (kset) ============================== */

/* The backup counts are read at BACKUP time through ReadParameterN
 * (SaveRegistryKeyEx and SavePortableDirBackup in kitty_regbackup.c), never cached at
 * startup: the store IS the running value, so the refresh reads the same
 * key the change wrote. Default 5, the backup code's own. */
int kitty_kset_backupcount(const char *key)
{
    char buf[32];
    if (ReadParameterN(INIT_SECTION, key, buf, sizeof(buf)))
        return atoi(buf);
    return 5;
}

/* ctrl->context.p names the key. */
void kitty_kset_backupcount_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                           void *data, int event)
{
    const char *key = (const char *)ctrl->context.p;
    char buf[32];

    if (event == EVENT_REFRESH) {
        sprintf(buf, "%d", kitty_kset_backupcount(key));
        /* dlg_editbox_set fires EVENT_VALCHANGE: unguarded, showing the
         * default would write it. */
        cfgwin_refreshing = 1;
        dlg_editbox_set(ctrl, dlg, buf);
        cfgwin_refreshing = 0;
    } else if (event == EVENT_VALCHANGE && !cfgwin_refreshing) {
        char *s = dlg_editbox_get(ctrl, dlg);
        if (s[0]) {
            /* Stored as it will be USED: the backup code treats anything
             * below 1 as off and caps at 50. */
            int v = atoi(s);
            if (v < 0) v = 0;
            if (v > 50) v = 50;
            sprintf(buf, "%d", v);
            WriteParameter(INIT_SECTION, (char *)key, buf);
        }
        sfree(s);
    }
}

/*
 * ---- The plain global keys of the settings tree -------------------------
 *
 * One table row per key: where it is stored, what kind of control shows it,
 * and how the RUNNING program is told. An Application panel has no Save, so
 * the store and the running value must change together or the panel snaps
 * back to the old value on its next refresh. A row with no running-value
 * hook is a key the program reads from the store at the moment of use, so
 * the store IS the running value.
 *
 * file_only rows are read and written in kitty.ini whatever the store mode:
 * the [Print] keys because their registry names would collide with the
 * config box's own (WriteParameter drops the section, so [Print] height and
 * [ConfigBox] height would be ONE registry value), and [FontFallback]
 * because its list is file-only and splitting one feature over two stores
 * helps nobody.
 */
/* KSET_MULTITEXT is KSET_TEXT for a field that holds LINES: the store keeps
 * the one-line escaped form (kitty_notes.c owns the escape pair), the field
 * shows the text itself. A plain KSET_TEXT would write the edit box's CRLF
 * straight into kitty.ini and cut the value in half at the first line end. */
enum kset_kind { KSET_BOOL, KSET_INT, KSET_SECS, KSET_TEXT, KSET_MULTITEXT,
                 KSET_CHOICE, KSET_FILE };
struct kset_choice { const char *name; const char *stored; int value; };
struct kset_key {
    const char *section, *key;
    enum kset_kind kind;
    bool file_only;
    int (*get)(void);              /* running value, or... */
    void (*set)(int);
    int *var;                      /* ...the variable itself */
    int min, max;                  /* KSET_INT: clamp; KSET_SECS: min, in ms */
    int dflt;                      /* shown when neither store nor program answers */
    const char *(*get_str)(void);  /* KSET_TEXT / KSET_FILE running value */
    void (*set_str)(const char *);
    const struct kset_choice *choices;
    int nchoices;
};

/* kitty.c, kitty_commun.c, kitty_bridge.c, kitty_image.c, winfont_fallback.c:
 * the running values. Declared here because this file has no
 * header for them. */
extern int  GetShortcutsFlag(void);            extern void SetShortcutsFlag(const int);
extern int  GetMouseShortcutsFlag(void);       extern void SetMouseShortcutsFlag(const int);
extern int  GetHyperlinkFlag(void);            extern void SetHyperlinkFlag(const int);
extern int  GetFunkeysDefault(void);           extern void SetFunkeysDefault(const int);
extern int  GetPasteSize(void);                extern void SetPasteSize(const int);
extern int  kitty_script_enabled(void);        extern void kitty_script_set_enabled(int);
extern int  kitty_broadcast_default(void);     extern void kitty_broadcast_set_enabled(int);
extern int  GetTitleBarFlag(void);             extern void SetTitleBarFlag(const int);
extern int  GetSizeFlag(void);                 extern void SetSizeFlag(const int);
extern int  GetWinrolFlag(void);               extern void SetWinrolFlag(const int);
extern int  GetCtrlTabFlag(void);              extern void SetCtrlTabFlag(const int);
extern int  GetTransparencyFlag(void);         extern void SetTransparencyEnabled(const int);
extern int  GetBackgroundImageFlag(void);      extern void SetBackgroundImageFlag(const int);
extern int  GetShrinkBitmapEnable(void);       extern void SetShrinkBitmapEnable(int);
extern char *GetIconFile(void);                extern void SetIconFile(const char *);
extern int  GetFontFallbackFlag(void);         extern void SetFontFallbackFlag(int);
extern int  GetAutoreconnectFlag(void);        extern void SetAutoreconnectFlag(const int);
extern int  GetReconnectDelay(void);           extern void SetReconnectDelay(const int);
extern int  GetProxyChainMax(void);            extern void SetProxyChainMax(const int);
extern int  GetUserPassSSHNoSave(void);        extern void SetUserPassSSHNoSave(const int);
extern int  GetModalErrorsFlag(void);          extern void SetModalErrorsFlag(const int);
extern int  GetModalNewHostKeyConfirmationFlag(void);
extern int  GetModalChangedHostKeyConfirmationFlag(void);
extern int  GetModalWeakKeyConfirmationFlag(void);
extern const char *get_sshver(void);           extern void set_sshver(const char *);
extern int  GetZModemFlag(void);               extern void SetZModemFlag(const int);
extern char *PSCPPath;                         extern void SetPSCPPath(const char *);

static const char *kset_get_iconfile(void) { return GetIconFile(); }

/* pscpport: unset has always meant the session's port (kitty_xfer.c), and the
 * label says "* = the session's port" - so the field shows "*" rather than an
 * empty box when nothing is set. Writing "*" back is
 * the same thing spelled out. */
static const char *kset_get_pscpport(void)
{
    static char buf[64];
    if (!ReadParameterN(INIT_SECTION, KI_PSCPPORT, buf, sizeof(buf)) || !buf[0])
        return "*";
    return buf;
}

const struct kset_key *kset_find(const char *key);
void kset_write(const struct kset_key *k, const char *text);
/* "Locate..." beside the download folder: the Explorer folder picker
 * (OpenDirName, kitty_winutil.c), the pick written like a typed value and the
 * box (the button's context) refreshed. */
static void kset_set_debug(int v) { debug_flag = v; }
static int  kset_get_debug(void) { return debug_flag; }

static const struct kset_choice kset_prompt_choices[] = {
    { KT_KSET_CH_POPUP, "yes", 1 }, { KT_KSET_CH_TERMINAL, "no", 0 } };

/* Terminal & Printing: the renderer and the frame pacing ([KiTTY] renderer /
 * framepace, read when a window is created / at startup). */
static const struct kset_choice kset_renderer_choices[] = {
    { KT_KSET_WD_RENDERER_GDI, "gdi", 0 }, { KT_KSET_WD_RENDERER_D2D, "d2d", 1 } };
static const struct kset_choice kset_framepace_choices[] = {
    { KT_KSET_WD_FP_AUTO, "auto", -1 }, { KT_KSET_WD_FP_30, "33", 33 },
    { KT_KSET_WD_FP_20, "50", 50 },     { KT_KSET_WD_FP_FIXED, "0", 0 } };

/* Direct2D needs Windows 8.1 (6.3); read from ntdll, which tells the truth
 * to a process whose manifest claims less. */
static bool kset_d2d_supported(void)
{
    typedef LONG (WINAPI *fn_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    static int known = -1;
    if (known < 0) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        fn_RtlGetVersion p = nt ? (fn_RtlGetVersion)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
        RTL_OSVERSIONINFOW vi;
        known = 0;
        memset(&vi, 0, sizeof(vi));
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (p && p(&vi) == 0)
            known = (vi.dwMajorVersion > 6 ||
                     (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 3)) ? 1 : 0;
    }
    return known == 1;
}
static void kset_set_framepace(int v)
{
    char buf[16];
    void kitty_pace_set_setting(const char *);
    if (v < 0) strcpy(buf, "auto"); else sprintf(buf, "%d", v);
    kitty_pace_set_setting(buf);
}
static void kset_set_renderer(int v)
{
    if (v == 1) {
        /* the store-aware writer every [KiTTY] switch goes through */
        SetTransparencyEnabled(0);
        WriteParameter(INIT_SECTION, KI_TRANSPARENCY, "no");
    }
}
static const struct kset_choice kset_funkeys_choices[] = {
    { KT_KSET_FK_XTERM216, "xterm216", FUNKY_XTERM_216 },   /* the built-in default */
    { KT_KSET_FK_TILDE,    "tilde",    FUNKY_TILDE },
    { KT_KSET_FK_LINUX,    "linux",    FUNKY_LINUX },
    { KT_KSET_FK_XTERMR6,  "xtermr6",  FUNKY_XTERM },
    { KT_KSET_FK_VT400,    "vt400",    FUNKY_VT400 },
    { KT_KSET_FK_VT100P,   "vt100p",   FUNKY_VT100P },
    { KT_KSET_FK_SCO,      "sco",      FUNKY_SCO } };
static const struct kset_choice kset_second_launcher_choices[] = {
    { KT_KSET_LA_SECOND_EXITS, "yes", 1 }, { KT_KSET_LA_SECOND_STARTS, "no", 0 } };
static const struct kset_choice kset_pwprot_choices[] = {
    { KT_KSET_STORAGE_PWPROT_MASTER, "master", 0 },
    { KT_KSET_STORAGE_PWPROT_DPAPI,  "dpapi",  1 },
    { KT_KSET_STORAGE_PWPROT_LEGACY, "legacy", 2 } };

static const struct kset_key kset_keys[] = {
    /* Terminal windows + Shortcuts */
    { INIT_SECTION, KI_SHORTCUTS,      KSET_BOOL, false, GetShortcutsFlag, SetShortcutsFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_MOUSESHORTCUTS, KSET_BOOL, false, GetMouseShortcutsFlag, SetMouseShortcutsFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_HYPERLINK,      KSET_BOOL, false, GetHyperlinkFlag, SetHyperlinkFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_FUNKEYS,        KSET_CHOICE, false, GetFunkeysDefault, SetFunkeysDefault, NULL, 0, 0, FUNKY_XTERM_216,
      NULL, NULL, kset_funkeys_choices, lenof(kset_funkeys_choices) },
    { INIT_SECTION, KI_PASTESIZE,      KSET_INT, false, GetPasteSize, SetPasteSize, NULL, 0, 100000000, 5120 },
    { INIT_SECTION, KI_DEBUG,          KSET_BOOL, false, kset_get_debug, kset_set_debug, NULL, 0, 0, 0 },
    /* Automation */
    { INIT_SECTION, KI_INITDELAY,      KSET_SECS, false, NULL, NULL, &init_delay, 0, 0, 2000 },
    { INIT_SECTION, KI_BCDELAY,        KSET_INT, false, NULL, NULL, &between_char_delay, 0, 10000, 0 },
    { INIT_SECTION, KI_INTERNALDELAY,  KSET_INT, false, NULL, NULL, &internal_delay, 1, 10000, 10 },
    { INIT_SECTION, KI_COMMANDDELAY,   KSET_SECS, false, NULL, NULL, &autocommand_delay, 5, 0, 50 },
    { INIT_SECTION, KI_SCRIPTMODE,     KSET_BOOL, false, kitty_script_enabled, kitty_script_set_enabled, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_SCRIPTFILEFILTER, KSET_TEXT, false, NULL, NULL, NULL, 0, 0, 0 },
    { INIT_SECTION, KI_SENDCMDMODE,    KSET_BOOL, false, kitty_broadcast_default, kitty_broadcast_set_enabled, NULL, 0, 0, 0 },
    /* The installation's group key. Empty in the store = derived (kitty.c);
     * the Broadcast leaf's own handler drives this row, not KSET_TEXTBOX,
     * because the field SHOWS the derived key when nothing is stored. */
    { INIT_SECTION, KI_SENDCMDGROUP,   KSET_TEXT, false, NULL, NULL, NULL, 0, 0, 0, NULL, kitty_broadcast_set_group },
    /* Window & display */
    { INIT_SECTION, KI_WINTITLE,       KSET_BOOL, false, GetTitleBarFlag, SetTitleBarFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_SIZE,           KSET_BOOL, false, GetSizeFlag, SetSizeFlag, NULL, 0, 0, 0 },
    { INIT_SECTION, KI_WINROLL,        KSET_BOOL, false, GetWinrolFlag, SetWinrolFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_CTRLTAB,        KSET_BOOL, false, GetCtrlTabFlag, SetCtrlTabFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_TRANSPARENCY,   KSET_BOOL, false, GetTransparencyFlag, SetTransparencyEnabled, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_RENDERER,       KSET_CHOICE, false, NULL, kset_set_renderer, NULL, 0, 0, 0,
      NULL, NULL, kset_renderer_choices, lenof(kset_renderer_choices) },
    { INIT_SECTION, KI_FRAMEPACE,      KSET_CHOICE, false, NULL, kset_set_framepace, NULL, 0, 0, -1,
      NULL, NULL, kset_framepace_choices, lenof(kset_framepace_choices) },
    { INIT_SECTION, KI_BGIMAGE,        KSET_BOOL, false, GetBackgroundImageFlag, SetBackgroundImageFlag, NULL, 0, 0, 0 },
    { INIT_SECTION, KI_SLIDEDELAY,     KSET_INT, false, NULL, NULL, &ImageSlideDelay, 0, 86400, 0 },
    { INIT_SECTION, KI_SHRINKBITMAP,   KSET_BOOL, false, GetShrinkBitmapEnable, SetShrinkBitmapEnable, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_ICONFILE,       KSET_FILE, false, NULL, NULL, NULL, 0, 0, 0, kset_get_iconfile, SetIconFile },
    { KI_SECTION_PRINT, KI_PRINT_HEIGHT,              KSET_INT, true, NULL, NULL, &PrintCharSize, 1, 10000, 100 },
    { KI_SECTION_PRINT, KI_PRINT_MAXLINE,             KSET_INT, true, NULL, NULL, &PrintMaxLinePerPage, 1, 1000, 60 },
    { KI_SECTION_PRINT, KI_PRINT_MAXCHAR,             KSET_INT, true, NULL, NULL, &PrintMaxCharPerLine, 1, 1000, 85 },
    { KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_ACTIVE,       KSET_BOOL, true, GetFontFallbackFlag, SetFontFallbackFlag, NULL, 0, 0, 1 },
    { KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_FALLBACK,     KSET_TEXT, true, NULL, NULL, NULL, 0, 0, 0, NULL, kitty_fontfallback_apply_list },
    /* Connection & reconnect */
    { INIT_SECTION, KI_AUTORECONNECT,  KSET_BOOL, false, GetAutoreconnectFlag, SetAutoreconnectFlag, NULL, 0, 0, 1 },
    { INIT_SECTION, KI_RECONNECTDELAY, KSET_INT, false, GetReconnectDelay, SetReconnectDelay, NULL, 1, 3600, 5 },
    { INIT_SECTION, KI_PROXYCHAINMAX,  KSET_INT, false, GetProxyChainMax, SetProxyChainMax, NULL, 1, 100, 5 },
    { INIT_SECTION, KI_USERPASSSSHNOSAVE, KSET_BOOL, false, GetUserPassSSHNoSave, SetUserPassSSHNoSave, NULL, 0, 0, 0 },
    { INIT_SECTION, KI_MODALERRORS,    KSET_BOOL, false, GetModalErrorsFlag, SetModalErrorsFlag, NULL, 0, 0, 0 },
    { INIT_SECTION, KI_MODALNEWHOSTKEYCONFIRMATION, KSET_CHOICE, false,
      GetModalNewHostKeyConfirmationFlag, SetModalNewHostKeyConfirmationFlag, NULL, 0, 0, 1,
      NULL, NULL, kset_prompt_choices, lenof(kset_prompt_choices) },
    { INIT_SECTION, KI_MODALCHANGEDHOSTKEYCONFIRMATION, KSET_CHOICE, false,
      GetModalChangedHostKeyConfirmationFlag, SetModalChangedHostKeyConfirmationFlag, NULL, 0, 0, 1,
      NULL, NULL, kset_prompt_choices, lenof(kset_prompt_choices) },
    { INIT_SECTION, KI_MODALWEAKKEYCONFIRMATION, KSET_CHOICE, false,
      GetModalWeakKeyConfirmationFlag, SetModalWeakKeyConfirmationFlag, NULL, 0, 0, 1,
      NULL, NULL, kset_prompt_choices, lenof(kset_prompt_choices) },
    { INIT_SECTION, KI_SSHVERSION,     KSET_TEXT, false, NULL, NULL, NULL, 0, 0, 0, get_sshver, set_sshver },
    /* Security > Application Notification: the note every KiTTY++ process
     * shows once, in the notice window (kitty/kitty_notes.c). */
    { INIT_SECTION, KI_NOTES,          KSET_MULTITEXT, false, NULL, NULL, NULL, 0, 0, 0,
      NULL, kitty_notes_set_running },
    { INIT_SECTION, KI_NOTESONCE,      KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 0 },
    /* Transfers & Tools */
    /* Shown from the STORE, not the running value: the startup search fills
     * PSCPPath in memory with what it found, and showing that here made a
     * cleared field look as if the path had come back. */
    { INIT_SECTION, KI_PSCPPATH,       KSET_FILE, false, NULL, NULL, NULL, 0, 0, 0, NULL, SetPSCPPath },
    { INIT_SECTION, KI_PSCPPORT,       KSET_TEXT, false, NULL, NULL, NULL, 0, 0, 0, kset_get_pscpport, NULL },
    { INIT_SECTION, KI_DOWNLOADDIR,   KSET_FILE, false, NULL, NULL, NULL, 0, 0, 0 },   /* a folder row (FILTER_FOLDERS): the handler must drive it as a file-select, not an edit box */
    { INIT_SECTION, KI_UPLOADDIR,     KSET_FILE, false, NULL, NULL, NULL, 0, 0, 0 },   /* a folder row too: the local Default Upload Folder */
    { INIT_SECTION, KI_TRANSFERNOTIFICATION, KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 1 },
    /* Transfers & Tools > OSC 5113 (kitten): the global defaults a session
     * follows until it sets its own (Connection > File-Transfer-Settings) */
    { INIT_SECTION, KI_TRANSFERMAXMB, KSET_INT, false, NULL, NULL, NULL, 0, 0, 1024 },   /* max 0 = unbounded; 0 = no limit */
    { INIT_SECTION, KI_TRANSFERFULLPATH, KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 0 },
    /* Launcher: a separate process reads these from the store when it starts */
    { KI_SECTION_LAUNCHER, KI_LAUNCHER_RELOAD,           KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 1 },
    { KI_SECTION_LAUNCHER, KI_LAUNCHER_ALREADYRUNCHECK,  KSET_CHOICE, false, NULL, NULL, NULL, 0, 0, 1,
      NULL, NULL, kset_second_launcher_choices, lenof(kset_second_launcher_choices) },
    { KI_SECTION_LAUNCHER, KI_LAUNCHER_EXITWITHWORKPLACE, KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 1 },
    { KI_SECTION_LAUNCHER, KI_LAUNCHER_NOTICESECONDS,    KSET_INT, false, NULL, NULL, NULL, 1, 600, 15 },
    /* Connection > ZModem: the global switch (the panel borrows the handler) */
    { INIT_SECTION, KI_ZMODEM,         KSET_BOOL, false, GetZModemFlag, SetZModemFlag, NULL, 0, 0, 1 },
    /* Storage & Backup: the folder-store password policy, read at save time */
    { INIT_SECTION, KI_PORTABLEPASSWORDPROTECTION, KSET_CHOICE, false, NULL, NULL, NULL, 0, 0, 0,
      NULL, NULL, kset_pwprot_choices, lenof(kset_pwprot_choices) },
    { INIT_SECTION, KI_WARNLEGACYPASSWORDUPGRADE, KSET_BOOL, false, NULL, NULL, NULL, 0, 0, 1 },
};

/* The banner preview under the client-version field (Connections leaf). */
dlgcontrol *kset_sshver_preview = NULL;
static void kset_show_banner(dlgparam *dlg)
{
    char banner[128], line[200];
    if (!kset_sshver_preview)
        return;
    kitty_ssh_banner_preview(banner, sizeof(banner));
    snprintf(line, sizeof(line), KT_KSET_CN_SSHVERSION_PREVIEW, banner);
    dlg_label_change(kset_sshver_preview, dlg, line);
}

const struct kset_key *kset_find(const char *key)
{
    for (size_t i = 0; i < lenof(kset_keys); i++)
        if (!strcmp(kset_keys[i].key, key))
            return &kset_keys[i];
    return NULL;
}

static int kset_read(const struct kset_key *k, char *buf, size_t size)
{
    buf[0] = '\0';
    if (k->file_only) {
        const char *ini = GetKittyIniFile();
        return ini && ini[0] && readINI(ini, k->section, k->key, buf, size) && buf[0];
    }
    return ReadParameterN(k->section, k->key, buf, size) != 0;
}

void kset_write(const struct kset_key *k, const char *text)
{
    if (k->file_only) {
        const char *ini = GetKittyIniFile();
        if (ini && ini[0] && !GetReadOnlyFlag())
            writeINI(ini, k->section, k->key, text);
    } else {
        WriteParameter(k->section, (char *)k->key, (char *)text);
    }
}

/*
 * A write held back until the typing stops.
 *
 * The configuration box has no Save and reports EVENT_VALCHANGE for every
 * keystroke, which is right for a port number and wrong for a paragraph: the
 * Application Notification would write the whole note to the store once per
 * character. Windows delivers EN_KILLFOCUS for a plain edit box to
 * windows/controls.c, which turns it into no handler event at all, so there
 * is nothing to write "on leaving the field" from; instead the text is
 * remembered here and written when one of three things happens - a second
 * passes with no further keystroke (the box's own one-second tick), the
 * panel is switched, or the box closes.
 *
 * One row at a time is enough: only one field defers, and moving to another
 * panel flushes before anything else can.
 */
static const struct kset_key *kset_deferred_key = NULL;
static char kset_deferred_text[4096];
static int kset_deferred_quiet = 0;    /* ticks since the last keystroke */

void kset_defer_write(const struct kset_key *k, const char *text)
{
    if (kset_deferred_key && kset_deferred_key != k)
        kset_write(kset_deferred_key, kset_deferred_text);
    kset_deferred_key = k;
    snprintf(kset_deferred_text, sizeof(kset_deferred_text), "%s", text);
    kset_deferred_quiet = 0;
}

/* Write it now. Called on a panel switch and when the box closes
 * (windows/dialog.c), and from the tick below once the typing has stopped. */
void kitty_cfgbox_flush_pending(void)
{
    if (!kset_deferred_key)
        return;
    kset_write(kset_deferred_key, kset_deferred_text);
    kset_deferred_key = NULL;
    kset_deferred_quiet = 0;
}

/* One second of quiet is the end of a burst of typing. */
static void kset_deferred_tick(void)
{
    if (!kset_deferred_key)
        return;
    if (++kset_deferred_quiet >= 1)
        kitty_cfgbox_flush_pending();
}

/* The running value, or the store, or the default - in that order. */
int kset_get_int(const struct kset_key *k)
{
    char buf[64];
    if (k->get) return k->get();
    if (k->var) return *k->var < k->min ? k->min : *k->var;   /* -1 = unset shows as the floor */
    if (kset_read(k, buf, sizeof(buf))) {
        if (k->kind == KSET_BOOL)
            return !stricmp(buf, "yes") ? 1 : !stricmp(buf, "no") ? 0 : k->dflt;
        return atoi(buf);
    }
    return k->dflt;
}

/* A switch whose checkbox says the OPPOSITE of what its key stores.
 * [Launcher] reload=yes lets the launcher rebuild its menu from the saved
 * sessions, and is the default; the box is the exception a person chooses,
 * "keep my hand-edited menu", so a tick stores reload=no. */
static bool kset_shown_reversed(const struct kset_key *k)
{
    return !strcmp(k->key, KI_LAUNCHER_RELOAD);
}

static void kset_set_int(const struct kset_key *k, int v)
{
    if (k->set) k->set(v);
    else if (k->var) *k->var = v;
}

void kitty_kset_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    const struct kset_key *k = (const struct kset_key *)ctrl->context.p;
    char buf[4096];

    if (!k)
        return;
    if (event == EVENT_REFRESH) {
        cfgwin_refreshing = 1;      /* dlg_*_set fires VALCHANGE - never write a refresh */
        switch (k->kind) {
          case KSET_BOOL:
            dlg_checkbox_set(ctrl, dlg, (kset_get_int(k) != 0) != kset_shown_reversed(k));
            break;
          case KSET_INT:
            sprintf(buf, "%d", kset_get_int(k));
            dlg_editbox_set(ctrl, dlg, buf);
            break;
          case KSET_SECS:
            sprintf(buf, "%g", kset_get_int(k) / 1000.0);
            dlg_editbox_set(ctrl, dlg, buf);
            break;
          case KSET_TEXT:
            if (k->get_str && k->get_str())
                snprintf(buf, sizeof(buf), "%s", k->get_str());
            else
                kset_read(k, buf, sizeof(buf));
            dlg_editbox_set(ctrl, dlg, buf);
            if (!strcmp(k->key, KI_SSHVERSION))
                kset_show_banner(dlg);
            break;
          case KSET_MULTITEXT: {
            /* The store holds one escaped line; the box shows the lines. */
            char stored[4096];
            stored[0] = '\0';
            /* A write this field is still holding back is the newer text:
             * a refresh that landed inside that second would otherwise put
             * the stale stored value back over what is being typed. */
            if (kset_deferred_key == k)
                snprintf(stored, sizeof(stored), "%s", kset_deferred_text);
            else
                kset_read(k, stored, sizeof(stored));
            kitty_notes_decode(stored, buf, sizeof(buf));
            dlg_editbox_set(ctrl, dlg, buf);
            break;
          }
          case KSET_FILE: {
            Filename *fn;
            if (k->get_str && k->get_str())
                snprintf(buf, sizeof(buf), "%s", k->get_str());
            else
                kset_read(k, buf, sizeof(buf));
            fn = filename_from_str(buf);
            dlg_filesel_set(ctrl, dlg, fn);
            filename_free(fn);
            break;
          }
          case KSET_CHOICE: {
            int cur = -2, i;
            if (k->get || k->var) {
                cur = kset_get_int(k);
            } else {
                /* no running value: match the stored text, else the default */
                cur = k->dflt;
                if (kset_read(k, buf, sizeof(buf)))
                    for (i = 0; i < k->nchoices; i++)
                        if (!stricmp(buf, k->choices[i].stored)) { cur = k->choices[i].value; break; }
            }
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < k->nchoices; i++) {
                const char *name = k->choices[i].name;
                /* Direct2D on a Windows below 8.1: offered, named as such,
                 * and refused when picked (a plain combo box cannot grey
                 * one entry) */
                if (!strcmp(k->key, KI_RENDERER) && k->choices[i].value == 1 &&
                    !kset_d2d_supported())
                    name = KT_KSET_WD_RENDERER_D2D_OLD;
                dlg_listbox_addwithid(ctrl, dlg, name, k->choices[i].value);
            }
            for (i = 0; i < k->nchoices; i++)
                if (k->choices[i].value == cur) { dlg_listbox_select(ctrl, dlg, i); break; }
            dlg_update_done(ctrl, dlg);
            break;
          }
        }
        cfgwin_refreshing = 0;
    } else if (event == EVENT_VALCHANGE && !cfgwin_refreshing) {
        switch (k->kind) {
          case KSET_BOOL: {
            int on = (dlg_checkbox_get(ctrl, dlg) != 0) != kset_shown_reversed(k) ? 1 : 0;
            kset_write(k, on ? "yes" : "no");
            kset_set_int(k, on);
            break;
          }
          case KSET_INT: {
            char *s = dlg_editbox_get(ctrl, dlg);
            if (s[0]) {
                /* stored as it will be USED: clamped to what the code accepts */
                int v = atoi(s);
                if (v < k->min) v = k->min;
                if (k->max > 0 && v > k->max) v = k->max;
                sprintf(buf, "%d", v);
                kset_write(k, buf);
                kset_set_int(k, v);
            }
            sfree(s);
            break;
          }
          case KSET_SECS: {
            char *s = dlg_editbox_get(ctrl, dlg);
            if (s[0]) {
                double secs = atof(s);
                int ms = (int)(secs * 1000.0 + 0.5);
                if (ms < k->min) ms = k->min;
                sprintf(buf, "%g", ms / 1000.0);
                kset_write(k, buf);
                kset_set_int(k, ms);
            }
            sfree(s);
            break;
          }
          case KSET_TEXT: {
            char *s = dlg_editbox_get(ctrl, dlg);
            kset_write(k, s);
            if (k->set_str) k->set_str(s);
            sfree(s);
            if (!strcmp(k->key, KI_SSHVERSION))
                kset_show_banner(dlg);
            break;
          }
          case KSET_MULTITEXT: {
            /* Every keystroke reports VALCHANGE, and a store write per
             * character typed into a paragraph of text is not what this
             * field should cost. So: the running copy is updated at once,
             * so anything reading it agrees with what is on screen, and the
             * WRITE is held back until the typing stops (kset_deferred_*
             * below: a second of quiet, a panel switch, or the box closing).
             */
            char *s = dlg_editbox_get(ctrl, dlg);
            kitty_notes_encode(s, buf, sizeof(buf));
            kset_defer_write(k, buf);
            if (k->set_str) k->set_str(s);
            sfree(s);
            break;
          }
          case KSET_FILE: {
            Filename *fn = dlg_filesel_get(ctrl, dlg);
            snprintf(buf, sizeof(buf), "%s", filename_to_str(fn));
            kset_write(k, buf);
            if (k->set_str) k->set_str(buf);
            filename_free(fn);
            break;
          }
          case KSET_CHOICE:
            break;              /* droplists report EVENT_SELCHANGE */
        }
    } else if (event == EVENT_SELCHANGE && k->kind == KSET_CHOICE && !cfgwin_refreshing) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx >= 0 && idx < k->nchoices) {
            if (!strcmp(k->key, KI_RENDERER) && k->choices[idx].value == 1 &&
                !kset_d2d_supported()) {
                /* snap back to GDI: nothing stored */
                cfgwin_refreshing = 1;
                dlg_listbox_select(ctrl, dlg, 0);
                cfgwin_refreshing = 0;
                return;
            }
            kset_write(k, k->choices[idx].stored);
            kset_set_int(k, k->choices[idx].value);
            /* Direct2D and window transparency no longer exclude each
             * other (a layerable window gets the blit-model swap chain,
             * paint-d2d.c), so the renderer choice leaves the
             * transparency checkbox alone. */
        }
    }
}
