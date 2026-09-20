/*
 * kitty_config_app.c - the Application tab of the configuration box: workplace proxy mode,
 * the helper-program paths, the host-key trust store, Config Window and
 * Security, the KiTTY++ Settings tree with the broadcast console, the
 * kitty.ini viewer, the shortcut editor, migration and the footers.
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
#include "kitty_osc52.h"      /* kitty_frame_restore_resting: the terminal frame follows the theme */
#include "kitty_defs.h"    /* KITTY_DEFAULT_SESSION */
#include "kitty_win.h"   /* SetTextToClipboard */
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
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
#include "kitty_exportbundle.h"
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

#define KITTY_WORKPLACE_STATE_ON  "Workplace proxy mode is ON for every connection."

/* How long "switch off after" can be set to. 0 means no timeout: the mode then
 * ends only when it is switched off or the launcher exits, which is still a
 * bounded promise because the launcher dies with the logon. */
static const struct { const char *label; unsigned int minutes; } wpmode_spans[] = {
    { KT_CFG_WPMODE_1H,                60 },
    { KT_CFG_WPMODE_2H,               120 },
    { KT_CFG_WPMODE_4H,               240 },
    { KT_CFG_WPMODE_8H,               480 },
    { KT_CFG_WPMODE_12H,              720 },
    { KT_CFG_WPMODE_LAUNCHER_EXIT,       0 },
};

/* The transparency checkbox of the same panel: Direct2D clears and greys it
 * (a translucent window cannot be painted by the GPU path; with both on,
 * GDI wins - the note on the panel and kitty.ini.example say so). */
static dlgcontrol *kset_transparency_ctrl;
#define KSET(key) P((void *)kset_find(key))

/* ==== Workplace proxy mode: the switch, its labels, the arming ========== */

/* Keep the state line telling the truth. Called from the button's refresh, so
 * it follows every action taken in this box; a change made elsewhere is picked
 * up by the poll below. */
void kitty_wpmode_state_label(struct wpmode_data *wd, dlgparam *dlg)
{
    char armed[256];
    if (!wd->state)
        return;
    if (kitty_workplace_query(armed, sizeof(armed))) {
        /* WARNING: No proxy name here, and nothing longer: the control was sized from
         * the OFF wording when the panel was built, so a longer line is CLIPPED
         * mid-sentence rather than wrapped. The name is in the droplist two rows
         * below anyway. */
        dlg_label_change(wd->state, dlg, KITTY_WORKPLACE_STATE_ON);
    } else {
        dlg_label_change(wd->state, dlg, KITTY_WORKPLACE_STATE_OFF);
    }
}

/* Enable or grey one workplace control, label included: a droplist is more
 * than one window, so every id the control owns is walked. Resolution goes
 * through kitty_cfg_item - the panel host owns these windows, so a bare
 * GetDlgItem on the dialog finds nothing. */
static void kitty_wpmode_enable_ctrl(dlgcontrol *ctrl, dlgparam *dlg, bool on)
{
    int i;
    if (!ctrl || !dlg)
        return;
    for (i = 0; i < dlg->nctrltrees; i++) {
        struct winctrl *c = winctrl_findbyctrl(dlg->controltrees[i], ctrl);
        if (c) {
            for (int k = 0; k < c->num_ids; k++) {
                HWND h = kitty_cfg_item(dlg->hwnd, c->base_id + k);
                if (h)
                    EnableWindow(h, on);
            }
            return;
        }
    }
}

/* The leaf is always in the tree; without a named proxy defined its controls
 * grey out and the info line says what to do about it. Live in both
 * directions - saving the first proxy on Named Proxies ungreys it, deleting
 * the last one greys it again (the poll watches the count). */
void kitty_wpmode_grey(struct wpmode_data *wd, dlgparam *dlg)
{
    bool have = kitty_has_proxy_definitions();
    if (wd->noproxy)
        dlg_label_change(wd->noproxy, dlg, have ? " " :
                         KT_WORKPLACE_PROXY_NEEDS_NAMED);
    kitty_wpmode_enable_ctrl(wd->list, dlg, have);
    kitty_wpmode_enable_ctrl(wd->hours, dlg, have);
    kitty_wpmode_enable_ctrl(wd->button, dlg, have);
}

void kitty_wpmode_button_label(dlgcontrol *ctrl, dlgparam *dlg)
{
    char armed[256], left[64];
    if (kitty_workplace_query(armed, sizeof(armed))) {
        /* The proxy is named in the droplist directly above, so the button says
         * only what pressing it does and how long the mode has left. */
        char *s;
        kitty_workplace_left_text(left, sizeof(left));
        s = left[0] ? dupprintf(KT_CFG_WPMODE_SWITCH_OFF_LEFT, left)
                    : dupstr(KT_CFG_WPMODE_SWITCH_OFF);
        dlg_label_change(ctrl, dlg, s);
        sfree(s);
    } else {
        dlg_label_change(ctrl, dlg, KT_WORKPLACE_PROXY_SWITCH);
    }
}

void kitty_wpmode_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    struct wpmode_data *wd = (struct wpmode_data *)ctrl->context.p;

    if (ctrl == wd->list) {
        if (event == EVENT_REFRESH) {
            /* Preselect what the mode is using, or failing that what it last
             * used - the answer to "switch it back on with what?". */
            char cur[256] = "", armed[256] = "", remembered[256] = "";
            int i, row = 0, sel = 0;
            /* Which to preselect: what the mode is using, else what it last
             * used. The entries say which is which, the same way the tray menu
             * does - a droplist of bare names cannot tell you why one of them
             * is showing. */
            if (!kitty_workplace_query(armed, sizeof(armed)))
                armed[0] = '\0';
            if (!ReadParameterN(INIT_SECTION, KI_WORKPLACEPROXY,
                                remembered, sizeof(remembered)))
                remembered[0] = '\0';
            /* A choice already made in this box wins: the list is also rebuilt
             * when the mode changes elsewhere (to move the "(in use)" tag), and
             * that must not drag the user's selection somewhere they did not
             * put it. */
            snprintf(cur, sizeof(cur), "%s",
                     (wd->picked && wd->name && wd->name[0]) ? wd->name :
                     (armed[0] ? armed : remembered));
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < MAX_PROXY && proxies[i].name; i++) {
                const char *tag = "";
                if (!strcmp(proxies[i].name, KITTY_PROXY_NONE) ||
                    !strcmp(proxies[i].name, KITTY_PROXY_SESSION))
                    continue;
                if (armed[0] && !strcmp(armed, proxies[i].name))
                    tag = KT_CFG_WPMODE_TAG_IN_USE;
                else if (remembered[0] && !strcmp(remembered, proxies[i].name))
                    tag = KT_CFG_WPMODE_TAG_LAST_USED;
                if (*tag) {
                    char *label = dupprintf("%s%s", proxies[i].name, tag);
                    dlg_listbox_add(ctrl, dlg, label);
                    sfree(label);
                } else {
                    dlg_listbox_add(ctrl, dlg, proxies[i].name);
                }
                if (cur[0] && !strcmp(cur, proxies[i].name))
                    sel = row;
                row++;
            }
            dlg_listbox_select(ctrl, dlg, sel);
            dlg_update_done(ctrl, dlg);
            /* The entries carry "(in use)"/"(last used)", so they are longer
             * than the closed control: widen the dropped-down list to fit. */
            kitty_dlg_droplist_fit(ctrl, dlg);
            {
                const char *n = kitty_pxload_name_at(sel);
                sfree(wd->name);
                wd->name = n ? dupstr(n) : NULL;
            }
        } else if (event == EVENT_SELCHANGE) {
            const char *n = kitty_pxload_name_at(dlg_listbox_index(ctrl, dlg));
            if (n) {
                sfree(wd->name);
                wd->name = dupstr(n);
                wd->picked = true;      /* a real choice, not a preselection */
                /* Remember the choice as soon as it is made, not only when the
                 * mode is switched on: this droplist and the launcher's menu
                 * read the same remembered selection, so picking here is also
                 * how you tell the launcher what to offer next time. */
                WriteParameter(INIT_SECTION, KI_WORKPLACEPROXY, wd->name);
            }
        }
        return;
    }

    if (ctrl == wd->hours) {
        if (event == EVENT_REFRESH) {
            char stored[32] = "";
            unsigned int m = 240;           /* a working afternoon */
            size_t i, sel = 0;
            if (ReadParameterN(INIT_SECTION, KI_WORKPLACEMINUTES, stored, sizeof(stored))
                && atoi(stored) >= 0)
                m = (unsigned int)atoi(stored);
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < lenof(wpmode_spans); i++) {
                dlg_listbox_add(ctrl, dlg, wpmode_spans[i].label);
                if (wpmode_spans[i].minutes == m)
                    sel = i;
            }
            dlg_listbox_select(ctrl, dlg, sel);
            dlg_update_done(ctrl, dlg);
            wd->minutes = wpmode_spans[sel].minutes;
        } else if (event == EVENT_SELCHANGE) {
            int i = dlg_listbox_index(ctrl, dlg);
            if (i >= 0 && i < (int)lenof(wpmode_spans)) {
                char m[32];
                wd->minutes = wpmode_spans[i].minutes;
                sprintf(m, "%u", wd->minutes);
                WriteParameter(INIT_SECTION, KI_WORKPLACEMINUTES, m);   /* remembered */
            }
        }
        return;
    }

    if (event == EVENT_REFRESH) {           /* the button and the state line */
        kitty_wpmode_button_label(ctrl, dlg);
        kitty_wpmode_state_label(wd, dlg);
        kitty_wpmode_grey(wd, dlg);
        return;
    }
    if (event != EVENT_ACTION)
        return;

    char armed[256];
    if (kitty_workplace_query(armed, sizeof(armed))) {
        /* On: ask the launcher to let go. */
        if (!kitty_workplace_request(0, 0))
            dlg_error_msg(dlg, KT_CFG_WPMODE_OFF_FAILED);
    } else {
        char m[32];
        if (!wd->name || !wd->name[0]) {
            dlg_beep(dlg);
            return;
        }
        /* Remember the SELECTION first: a running launcher arms from it, and a
         * launcher started below is handed the same name. */
        WriteParameter(INIT_SECTION, KI_WORKPLACEPROXY, wd->name);
        sprintf(m, "%u", wd->minutes);
        WriteParameter(INIT_SECTION, KI_WORKPLACEMINUTES, m);
        if (!kitty_workplace_request(1, wd->minutes) &&
            !kitty_workplace_start_launcher(wd->name, wd->minutes))
            dlg_error_msg(dlg, KT_CFG_WPMODE_ON_FAILED);
    }
    dlg_refresh(NULL, dlg);
}

/* "Edit" button beside the Proxy-choice droplist (and at the foot of the
 * Connection/Proxy panel): open the named-proxy editor, then refresh so the
 * droplist re-lists the current set. Note: whether the Session-panel droplist
 * *exists* is fixed when the config box is built (kitty_proxy_choice_shown() in
 * auto mode), so crossing 0<->1 proxies only takes effect on the next config-box
 * open; the editor shows a one-time "reopen the configuration" note in that
 * case (kitty_proxy_gui.c). Making it live is a scoped follow-up (would require
 * rebuilding the whole ctrlbox). */

/* ==== Helper-program paths and the update switch ======================== */

/* The update check: an application setting in kitty.ini, so it reads and
 * writes there rather than through the session's Conf. Immediate - there is no
 * Save on an application setting, and nothing else in the box would carry it. */
static void kitty_checkupdate_global_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                             void *data, int event)
{
    if (event == EVENT_REFRESH)
        dlg_checkbox_set(ctrl, dlg, kitty_check_update_enabled() != 0);
    else if (event == EVENT_VALCHANGE)
        kitty_set_check_update_enabled(dlg_checkbox_get(ctrl, dlg) ? 1 : 0);
}

void kitty_proxyedit_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    if (event == EVENT_ACTION) {
        Conf *conf = (Conf *)data;
        /* Open the editor ON the definition currently chosen in the override
         * droplist this button sits beside - that is almost always the one the
         * user means to edit. The two built-ins are not definitions, so they pass
         * nothing and the editor opens on defaults as before. */
        const char *sel = conf ? conf_get_str(conf, CONF_proxyselection) : NULL;
        if (sel && (!strcmp(sel, KITTY_PROXY_SESSION) ||
                    !strcmp(sel, KITTY_PROXY_NONE)))
            sel = NULL;
        /* The definitions now have a PANEL of their own on the Application tab,
         * so this button is a way there rather than a second editor. The button
         * stays where it is: it is an entry point, not a setting that moved,
         * and someone editing a session's proxy is exactly who wants it. */
        kitty_proxy_panel_preselect(sel);
        kitty_cfg_goto_panel("Application/Named Proxies");
    }
}

/* Where an installer puts a helper, offered as a display HINT when kitty.ini
 * stores no path for it - shown, never written (the refresh guard below).
 * The probes run in this order: %ProgramFiles%, %ProgramFiles(x86)%, then
 * the per-user %LOCALAPPDATA%\Programs; the first file that exists wins.
 * Helpers without a row (rz, sz) get no hint. Mirrors SearchWinSCP() in
 * kitty.c for WinSCP. */
static const struct { const char *key; const char *under_programs; } kitty_toolpath_hints[] = {
    { KI_WINSCPPATH,    "WinSCP\\WinSCP.exe" },
    { KI_FILEZILLAPATH, "FileZilla FTP Client\\filezilla.exe" },
};
static void kitty_toolpath_hint(const char *key, char *buffer, size_t size)
{
    const char *pf = getenv("ProgramFiles");
    const char *pf86 = getenv("ProgramFiles(x86)");
    const char *local = getenv("LOCALAPPDATA");
    size_t i;
    buffer[0] = '\0';
    for (i = 0; i < lenof(kitty_toolpath_hints); i++) {
        const char *rel = kitty_toolpath_hints[i].under_programs;
        if (strcmp(kitty_toolpath_hints[i].key, key)) continue;
        if (pf) {
            snprintf(buffer, size, "%s\\%s", pf, rel);
            if (!existfile(buffer)) buffer[0] = '\0';
        }
        if (!buffer[0] && pf86) {
            snprintf(buffer, size, "%s\\%s", pf86, rel);
            if (!existfile(buffer)) buffer[0] = '\0';
        }
        if (!buffer[0] && local) {
            snprintf(buffer, size, "%s\\Programs\\%s", local, rel);
            if (!existfile(buffer)) buffer[0] = '\0';
        }
        return;
    }
}

/*
 * A helper program's path is a GLOBAL app setting in kitty.ini ([KiTTY]
 * WinSCPPath, FileZillaPath, rz and sz), NOT a per-session CONF_ key - so it
 * cannot use conf_filesel_handler. On REFRESH the stored path is shown, or,
 * for a helper with a hint row above, the installer's location when nothing
 * is stored yet (display only, never written on refresh). On VALCHANGE
 * whatever the user selected or typed is persisted. ctrl->context.p names
 * the kitty.ini key. The rz and sz paths used to be per-session
 * (CONF_rzcommand / CONF_szcommand), so the path went into every saved
 * session and had to be set again for each host.
 */
static void kitty_toolpath_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    const char *key = (const char *)ctrl->context.p;
    /* dlg_editbox_set() / dlg_filesel_set() fire a re-entrant EVENT_VALCHANGE
     * (see the autopw handler note above); this guard stops the refresh-time
     * value - a hint in particular - from being written back to kitty.ini, so
     * only genuine user edits persist. */
    static int refreshing = 0;

    if (event == EVENT_REFRESH) {
        char buffer[4096];
        Filename *fn;
        buffer[0] = '\0';
        refreshing = 1;
        if (!ReadParameterN(INIT_SECTION, (char *)key, buffer, sizeof(buffer)) ||
            !buffer[0])
            kitty_toolpath_hint(key, buffer, sizeof(buffer));
        fn = filename_from_str(buffer);
        dlg_filesel_set(ctrl, dlg, fn);
        filename_free(fn);
        refreshing = 0;
    } else if (event == EVENT_VALCHANGE) {
        Filename *fn;
        char val[4096];
        if (refreshing)
            return;
        fn = dlg_filesel_get(ctrl, dlg);
        snprintf(val, sizeof(val), "%s", filename_to_str(fn));
        WriteParameter(INIT_SECTION, (char *)key, val);
        filename_free(fn);
    }
}

/* (The Proxy panel's pre-set loader used to be pinned to the BOTTOM of the
 * panel area, which left a blank band between the session's own fields and
 * the loader. It follows the layout now, directly under them.) */

void kitty_config_footer_pin(const char *path);   /* defined below */

/* ==== The kitty.ini view's placement (the view itself is further down) ==== */

/* The kitty.ini view's Edit button: flush with the view's right edge. The
 * layout gives it a column that widens with the window and leaves the button
 * at the column's left, which reads as "somewhere right-ish". Placed after
 * the layout, from the rectangles Windows produced, like the session-list
 * buttons. */
void kitty_iniview_place(void)
{
    struct iniview_data *iv = kitty_iniview_active;
    HWND hview, hbtn;
    RECT vr, br;
    POINT pt;

    if (!iv || !iv->view || !iv->editbtn)
        return;
    hview = kitty_cfg_ctrl_hwnd(iv->view);
    hbtn = kitty_cfg_ctrl_hwnd(iv->editbtn);
    if (!hview || !hbtn || !GetWindowRect(hview, &vr) || !GetWindowRect(hbtn, &br))
        return;
    pt.x = vr.right - (br.right - br.left);
    pt.y = br.top;
    ScreenToClient(GetParent(hbtn), &pt);
    SetWindowPos(hbtn, NULL, pt.x, pt.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- Security > Host keys: the trust store, listed ----------------------- */


static int hk_selected(struct hk_data *hk, dlgparam *dlg, int *idx, int max);

static struct hk_verdict *hk_verdict_find(struct hk_data *hk,
                                          const struct kitty_hostkey_entry *e)
{
    for (int i = 0; i < hk->nverdicts; i++) {
        struct hk_verdict *v = &hk->verdicts[i];
        if (v->port == e->port && !strcmp(v->keytype, e->keytype) &&
            !strcmp(v->host, e->host))
            return v;
    }
    return NULL;
}

static void hk_verdict_clear(struct hk_verdict *v)
{
    sfree(v->status); sfree(v->sha256); sfree(v->md5); sfree(v->error); sfree(v->when);
    v->status = v->sha256 = v->md5 = v->error = v->when = NULL;
}

/* The verdict slot for a key, made if absent (its strings cleared). */
static struct hk_verdict *hk_verdict_slot(struct hk_data *hk,
                                          const char *host, int port,
                                          const char *keytype)
{
    struct hk_verdict *v;
    struct kitty_hostkey_entry probe;
    probe.host = (char *)host; probe.port = port; probe.keytype = (char *)keytype;
    v = hk_verdict_find(hk, &probe);
    if (!v) {
        sgrowarray(hk->verdicts, hk->verdicts_alloc, hk->nverdicts);
        v = &hk->verdicts[hk->nverdicts++];
        memset(v, 0, sizeof(*v));
        v->host = dupstr(host); v->port = port; v->keytype = dupstr(keytype);
    } else
        hk_verdict_clear(v);
    return v;
}

static void hk_verdict_set(struct hk_verdict *v, const char *status,
                           const char *sha256, const char *md5,
                           const char *error, const char *when)
{
    hk_verdict_clear(v);
    v->status = dupstr(status);
    v->sha256 = dupstr(sha256 ? sha256 : "");
    v->md5 = dupstr(md5 ? md5 : "");
    v->error = dupstr(error ? error : "");
    v->when = dupstr(when ? when : "");
}

/* An ISO stamp "YYYY-MM-DDThh:mm:ss" -> the date "YYYY-MM-DD" (the column)
 * or "YYYY-MM-DD hh:mm:ss" (the detail); "-" for none. Static buffer. */
static const char *hk_stamp(const char *iso, bool date_only)
{
    static char buf[32];
    if (!iso || !iso[0])
        return "-";
    strncpy(buf, iso, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (date_only && strlen(buf) > 10)
        buf[10] = '\0';
    else if (buf[10] == 'T')
        buf[10] = ' ';
    return buf;
}

/* The klink call that repeats a row's check, for the detail box. */
static char *hk_klink_line(const struct kitty_hostkey_entry *e)
{
    return strchr(e->host, ':') ?
        dupprintf("klink -scan -t %s [%s]:%d", e->type_display, e->host, e->port) :
        dupprintf("klink -scan -t %s %s:%d", e->type_display, e->host, e->port);
}

/* The detail box: the selected key in full, or the hint when none is. */
static void hk_show_detail(struct hk_data *hk, dlgparam *dlg,
                           const struct kitty_hostkey_entry *e)
{
    strbuf *sb;
    if (!hk->detail)
        return;
    sb = strbuf_new();
    if (!e)
        put_dataz(sb, KT_HK_DETAIL_NONE);
    else {
        struct hk_verdict *v = hk_verdict_find(hk, e);
        char *klink = hk_klink_line(e);
        char *first = dupstr(hk_stamp(e->first_seen, false));
        put_fmt(sb, KT_HK_DETAIL, e->host, e->port, e->type_display, e->bits,
                e->sha256[0] ? e->sha256 : "-", e->md5[0] ? e->md5 : "-",
                first, hk_stamp(e->last_written, false));
        put_fmt(sb, "\r\n" KT_HK_DETAIL_CHECK, klink);
        if (v && v->status) {
            char *when = dupstr(hk_stamp(v->when, false));
            put_fmt(sb, "\r\n" KT_HK_DETAIL_VERIFIED, when, v->status);
            if (v->sha256[0])
                put_fmt(sb, "\r\n" KT_HK_DETAIL_PRESENTED, v->sha256, v->md5);
            if (v->error[0])
                put_fmt(sb, "\r\n%s", v->error);
            sfree(when);
        }
        sfree(first); sfree(klink);
    }
    dlg_editbox_set(hk->detail, dlg, sb->s);
    strbuf_free(sb);
}

/* One row's text, the column order of KT_HK_COL_HEAD. Caller frees. */
static char *hk_row_text(struct hk_data *hk, const struct kitty_hostkey_entry *e)
{
    const struct hk_verdict *v = hk_verdict_find(hk, e);
    /* The column is headed SHA256, so the hash goes in bare; the detail box
     * and Copy carry the "SHA256:" form. */
    const char *sha = e->sha256[0] ? e->sha256 : "-";
    char *first = dupstr(hk_stamp(e->first_seen, true));
    char *type = e->bits ? dupprintf("%s %d", e->type_display, e->bits) : dupstr(e->type_display);
    char *row;
    if (!strncmp(sha, "SHA256:", 7)) sha += 7;
    row = dupprintf("%s:%d\t%s\t%s\t%s\t%s\t%s", e->host, e->port, type, sha, first,
                    hk_stamp(e->last_written, true),
                    v && v->status ? v->status : "-");
    sfree(first); sfree(type);
    return row;
}

/* ---- sorting: the header row is the control -------------------------------- */

static struct hk_data *hk_sort_hk;      /* qsort has no context argument */

static int hk_cmp_str(const char *a, const char *b)
{
    int c = stricmp(a ? a : "", b ? b : "");
    return c ? c : strcmp(a ? a : "", b ? b : "");
}

static int hk_sort_cmp(const void *av, const void *bv)
{
    struct hk_data *hk = hk_sort_hk;
    const struct kitty_hostkey_entry *a = &hk->keys->items[*(const int *)av];
    const struct kitty_hostkey_entry *b = &hk->keys->items[*(const int *)bv];
    int c = 0;
    switch (hk->sort_col) {
      case 1: c = hk_cmp_str(a->type_display, b->type_display);
              if (!c) c = a->bits - b->bits;
              break;
      case 2: c = strcmp(a->sha256, b->sha256); break;
      case 3: c = strcmp(a->first_seen, b->first_seen); break;
      case 4: c = strcmp(a->last_written, b->last_written); break;
      case 5: {
        const struct hk_verdict *va = hk_verdict_find(hk, a), *vb = hk_verdict_find(hk, b);
        c = hk_cmp_str(va && va->status ? va->status : "", vb && vb->status ? vb->status : "");
        break;
      }
      default: break;
    }
    if (!c) c = hk_cmp_str(a->host, b->host);       /* host, port, type: the default */
    if (!c) c = a->port - b->port;
    if (!c) c = hk_cmp_str(a->type_display, b->type_display);
    return hk->sort_desc ? -c : c;
}

static void hk_fill(struct hk_data *hk, dlgparam *dlg)
{
    int i, *order;
    kitty_hostkeys_free(hk->keys);
    hk->keys = kitty_hostkeys_enumerate();
    order = snewn(hk->keys->n + 1, int);
    for (i = 0; i < hk->keys->n; i++) order[i] = i;
    hk_sort_hk = hk;
    qsort(order, hk->keys->n, sizeof(int), hk_sort_cmp);
    dlg_update_start(hk->listbox, dlg);
    dlg_listbox_clear(hk->listbox, dlg);
    dlg_listbox_addwithid(hk->listbox, dlg, KT_HK_COL_HEAD, -1);
    for (i = 0; i < hk->keys->n; i++) {
        char *row = hk_row_text(hk, &hk->keys->items[order[i]]);
        dlg_listbox_addwithid(hk->listbox, dlg, row, order[i]);
        sfree(row);
    }
    dlg_update_done(hk->listbox, dlg);
    sfree(order);
    hk_show_detail(hk, dlg, NULL);
    if (hk->banner && !hk->run) {
        char *line = dupprintf(KT_HK_COUNT, hk->keys->n, hk->keys->n == 1 ? "" : "s");
        dlg_label_change(hk->banner, dlg, line);
        sfree(line);
    }
}

/* Rewrite one row in place (a verdict came in) - the selection stays. */
static void hk_update_row(struct hk_data *hk, int idx)
{
    HWND h = kitty_cfg_ctrl_hwnd(hk->listbox);
    int n, r;
    if (!h || !hk->keys || idx < 0 || idx >= hk->keys->n) return;
    n = (int)SendMessage(h, LB_GETCOUNT, 0, 0);
    for (r = 1; r < n; r++) {
        if ((int)SendMessage(h, LB_GETITEMDATA, r, 0) == idx) {
            char *row = hk_row_text(hk, &hk->keys->items[idx]);
            bool sel = SendMessage(h, LB_GETSEL, r, 0) > 0;
            int top = (int)SendMessage(h, LB_GETTOPINDEX, 0, 0);
            SendMessage(h, WM_SETREDRAW, FALSE, 0);
            SendMessage(h, LB_DELETESTRING, r, 0);
            SendMessage(h, LB_INSERTSTRING, r, (LPARAM)row);
            SendMessage(h, LB_SETITEMDATA, r, idx);
            if (sel) SendMessage(h, LB_SETSEL, TRUE, r);
            SendMessage(h, LB_SETTOPINDEX, top, 0);
            SendMessage(h, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(h, NULL, TRUE);
            sfree(row);
            break;
        }
    }
}

/* The column under the mouse, for a click on the header row. */
static int hk_column_at_cursor(struct hk_data *hk)
{
    HWND h = kitty_cfg_ctrl_hwnd(hk->listbox);
    POINT pt; RECT r;
    int width, x, acc = 0, i;
    if (!h || !GetCursorPos(&pt) || !ScreenToClient(h, &pt) || !GetClientRect(h, &r))
        return 0;
    width = r.right - r.left;
    x = pt.x - r.left;
    if (width <= 0) return 0;
    for (i = 0; i < hk->listbox->listbox.ncols - 1; i++) {
        acc += hk->listbox->listbox.percentages[i];
        if (x < width * acc / 100) return i;
    }
    return hk->listbox->listbox.ncols - 1;
}

/* The MISMATCH row in red (controls.c asks per row). */
static bool hk_row_ink(dlgcontrol *ctrl, int id, bool dark, COLORREF *ink)
{
    struct hk_data *hk = (struct hk_data *)ctrl->context.p;
    const struct hk_verdict *v;
    if (!hk || !hk->keys || id < 0 || id >= hk->keys->n) return false;
    v = hk_verdict_find(hk, &hk->keys->items[id]);
    if (!v || !v->status || strcmp(v->status, "MISMATCH")) return false;
    *ink = dark ? RGB(255, 110, 110) : RGB(192, 0, 0);
    return true;
}

/* ---- Verify: klink in the background, judged here ------------------------- */

static void hk_run_stop(struct hk_data *hk)
{
    if (hk->timer) { KillTimer(NULL, hk->timer); hk->timer = 0; }
    if (hk->run) { kitty_hkv_release(hk->run); hk->run = NULL; }
}

/* A finished job -> its row's verdict. klink's own stored/new/MISMATCH is
 * about the REGISTRY store; the key text is judged against OUR store. */
static void hk_judge(struct hk_data *hk, const struct kitty_hkv_job *j)
{
    struct hk_verdict *v = hk_verdict_slot(hk, j->host, j->port, j->keytype);
    const char *status;
    if (j->key[0]) {
        int cmp = check_stored_host_key(j->host, j->port, j->keytype, j->key);
        status = cmp == 0 ? "OK" : cmp == 2 ? "MISMATCH" : "not stored";
    } else
        status = j->status;             /* not offered / unreachable / no klink / klink failed */
    hk_verdict_set(v, status, j->sha256, j->md5, j->error, j->when);
}

static int hk_index_of(struct hk_data *hk, const char *host, int port, const char *keytype)
{
    for (int i = 0; hk->keys && i < hk->keys->n; i++) {
        const struct kitty_hostkey_entry *e = &hk->keys->items[i];
        if (e->port == port && !strcmp(e->keytype, keytype) && !strcmp(e->host, host))
            return i;
    }
    return -1;
}

static struct hk_data *hk_timer_hk;     /* the one run at a time */

static void CALLBACK hk_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    struct hk_data *hk = hk_timer_hk;
    struct kitty_hkv_job *jobs;
    int n, i, ok = 0, bad = 0, other = 0, seen = 0;
    if (!hk || !hk->run || !hk->dlg) return;
    jobs = kitty_hkv_jobs(hk->run, &n);
    for (i = 0; i < n; i++) {
        LONG st = InterlockedCompareExchange(&jobs[i].state, 0, 0);
        if (st == KHKV_RUNNING && hk->shown_state[i] != KHKV_RUNNING) {
            struct hk_verdict *v = hk_verdict_slot(hk, jobs[i].host, jobs[i].port, jobs[i].keytype);
            char *line = dupprintf(KT_HK_VERIFYING, jobs[i].host, jobs[i].port);
            hk_verdict_set(v, KT_HK_RUNNING, "", "", "", "");
            hk_update_row(hk, hk_index_of(hk, jobs[i].host, jobs[i].port, jobs[i].keytype));
            dlg_label_change(hk->banner, hk->dlg, line);
            sfree(line);
            hk->shown_state[i] = KHKV_RUNNING;
        } else if (st == KHKV_DONE && hk->shown_state[i] != KHKV_DONE) {
            int idx = hk_index_of(hk, jobs[i].host, jobs[i].port, jobs[i].keytype);
            hk_judge(hk, &jobs[i]);
            hk_update_row(hk, idx);
            hk->shown_state[i] = KHKV_DONE;
            /* the detail follows a selected row's verdict as it lands */
            {
                int sel[2];
                if (hk_selected(hk, hk->dlg, sel, 2) == 1 && sel[0] == idx)
                    hk_show_detail(hk, hk->dlg, &hk->keys->items[idx]);
            }
        }
        if (st == KHKV_DONE) seen++;
    }
    if (seen == n) {
        for (i = 0; i < hk->nverdicts; i++) {
            const char *s = hk->verdicts[i].status;
            if (!s) continue;
            if (!strcmp(s, "OK")) ok++;
            else if (!strcmp(s, "MISMATCH")) bad++;
            else other++;
        }
        {
            char *line = dupprintf(KT_HK_VERIFIED_SUMMARY, n, n == 1 ? "" : "s", ok, bad, other);
            dlg_label_change(hk->banner, hk->dlg, line);
            sfree(line);
        }
        hk_run_stop(hk);
    }
}

/* Start verifying the given entries. */
static void hk_verify(struct hk_data *hk, dlgparam *dlg, const int *idx, int n)
{
    struct kitty_hkv_job *jobs;
    int i;
    if (hk->run) { dlg_label_change(hk->banner, dlg, KT_HK_VERIFY_BUSY); return; }
    if (n <= 0) return;
    jobs = snewn(n, struct kitty_hkv_job);
    memset(jobs, 0, n * sizeof(*jobs));
    for (i = 0; i < n; i++) {
        const struct kitty_hostkey_entry *e = &hk->keys->items[idx[i]];
        jobs[i].host = e->host; jobs[i].port = e->port; jobs[i].keytype = e->keytype;
    }
    hk->run = kitty_hkv_start(jobs, n);
    sfree(jobs);
    if (!hk->run) { dlg_label_change(hk->banner, dlg, KT_HK_VERIFY_NOTHREAD); return; }
    sfree(hk->shown_state);
    hk->shown_state = snewn(n, LONG);
    for (i = 0; i < n; i++) hk->shown_state[i] = KHKV_PENDING;
    hk->dlg = dlg;
    hk_timer_hk = hk;
    hk->timer = SetTimer(NULL, 0, 250, hk_timer_proc);
    dlg_label_change(hk->banner, dlg, KT_HK_VERIFY_STARTING);
}

/* The selected entries, in list order. Returns how many; fills idx[]. */
static int hk_selected(struct hk_data *hk, dlgparam *dlg, int *idx, int max)
{
    int i, n = 0;
    for (i = 0; hk->keys && i <= hk->keys->n && n < max; i++) {
        int id;
        if (!dlg_listbox_issel(hk->listbox, dlg, i))
            continue;
        id = dlg_listbox_getid(hk->listbox, dlg, i);
        if (id >= 0 && id < hk->keys->n)
            idx[n++] = id;
    }
    return n;
}

static void kitty_hk_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    struct hk_data *hk = (struct hk_data *)ctrl->context.p;
    int which = ctrl->context2.i;          /* 0 list, 1 copy, 2 delete, 3 detail, 4 verify */
    int idx[4096], n, i;

    if (!hk) return;
    hk->dlg = dlg;
    switch (which) {
      case 0:
        if (event == EVENT_REFRESH) {
            hk_fill(hk, dlg);
        } else if (event == EVENT_SELCHANGE) {
            HWND h = kitty_cfg_ctrl_hwnd(ctrl);
            if (h && SendMessage(h, LB_GETSEL, 0, 0) > 0) {
                /* the header row. A MOUSE click there sorts by that column;
                 * the arrow keys landing on it (extended selection selects
                 * the caret row) just step back to the first key. */
                RECT hr;
                POINT pt;
                bool mouse = GetKeyState(VK_LBUTTON) < 0 && GetCursorPos(&pt) &&
                    ScreenToClient(h, &pt) &&
                    SendMessage(h, LB_GETITEMRECT, 0, (LPARAM)&hr) != LB_ERR &&
                    PtInRect(&hr, pt);
                SendMessage(h, LB_SETSEL, FALSE, 0);
                if (mouse) {
                    int col = hk_column_at_cursor(hk);
                    if (col == hk->sort_col) hk->sort_desc = !hk->sort_desc;
                    else { hk->sort_col = col; hk->sort_desc = false; }
                    hk_fill(hk, dlg);
                    break;
                }
                if (SendMessage(h, LB_GETCOUNT, 0, 0) > 1) {
                    SendMessage(h, LB_SETSEL, TRUE, 1);
                    SendMessage(h, LB_SETCARETINDEX, 1, 0);
                }
            }
            n = hk_selected(hk, dlg, idx, lenof(idx));
            hk_show_detail(hk, dlg, n == 1 ? &hk->keys->items[idx[0]] : NULL);
        }
        break;
      case 3:                              /* the detail box: display only */
        break;
      case 4:                              /* Verify: the selection, else all */
        if (event == EVENT_ACTION) {
            n = hk_selected(hk, dlg, idx, lenof(idx));
            if (!n)
                for (n = 0; n < hk->keys->n && n < (int)lenof(idx); n++) idx[n] = n;
            if (!n) { dlg_label_change(hk->banner, dlg, KT_HK_VERIFY_EMPTY); break; }
            hk_verify(hk, dlg, idx, n);
        }
        break;
      case 1:                              /* Copy */
        if (event == EVENT_ACTION) {
            strbuf *sb;
            n = hk_selected(hk, dlg, idx, lenof(idx));
            if (!n) { dlg_label_change(hk->banner, dlg, KT_HK_NOSEL); break; }
            sb = strbuf_new();
            for (i = 0; i < n; i++) {
                const struct kitty_hostkey_entry *e = &hk->keys->items[idx[i]];
                const struct hk_verdict *v = hk_verdict_find(hk, e);
                char *line = kitty_hostkey_describe(e);
                put_fmt(sb, "%s  %s  %s", line,
                        e->first_seen[0] ? e->first_seen : "-",
                        e->last_written[0] ? e->last_written : "-");
                if (v && v->status)
                    put_fmt(sb, "  %s", v->status);
                put_dataz(sb, "\r\n");
                sfree(line);
            }
            SetTextToClipboard(sb->s);
            strbuf_free(sb);
            {
                char *msg = dupprintf(KT_HK_COPIED, n, n == 1 ? "" : "s");
                dlg_label_change(hk->banner, dlg, msg);
                sfree(msg);
            }
        }
        break;
      case 2:                              /* Delete */
        if (event == EVENT_ACTION) {
            char *q;
            int done = 0;
            n = hk_selected(hk, dlg, idx, lenof(idx));
            if (!n) { dlg_label_change(hk->banner, dlg, KT_HK_NOSEL); break; }
            if (n == 1) {
                const struct kitty_hostkey_entry *e = &hk->keys->items[idx[0]];
                char *hp = dupprintf("%s:%d (%s)", e->host, e->port, e->type_display);
                q = dupprintf(KT_HK_CONFIRM_DELETE, hp);
                sfree(hp);
            } else
                q = dupprintf(KT_HK_CONFIRM_DELETE_N, n);
            if (MessageBoxA(kitty_cfg_modal_owner(), q, KT_CAP_KITTY,
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                for (i = 0; i < n; i++) {
                    const struct kitty_hostkey_entry *e = &hk->keys->items[idx[i]];
                    if (kitty_hostkey_delete(e->host, e->port, e->keytype))
                        done++;
                }
                hk_fill(hk, dlg);
                {
                    char *msg = dupprintf(KT_HK_DELETED, done, done == 1 ? "" : "s");
                    dlg_label_change(hk->banner, dlg, msg);
                    sfree(msg);
                }
            }
            sfree(q);
        }
        break;
    }
}

/* The box is closing (windows/dialog.c, before its memory goes): a running
 * verification keeps its worker thread - it frees the run itself when the
 * last klink returns - but the timer that polled it stops here, and the
 * panel state is forgotten. */
static void hk_box_closing(void)
{
    struct hk_data *hk = kitty_hk_active;
    if (hk) {
        hk_run_stop(hk);
        for (int i = 0; i < hk->nverdicts; i++) {
            hk_verdict_clear(&hk->verdicts[i]);
            sfree(hk->verdicts[i].host); sfree(hk->verdicts[i].keytype);
        }
        sfree(hk->verdicts);
        sfree(hk->shown_state);
        kitty_hostkeys_free(hk->keys);
        hk->verdicts = NULL; hk->keys = NULL; hk->shown_state = NULL;
        hk->nverdicts = 0; hk->verdicts_alloc = 0;
    }
    hk_timer_hk = NULL;
    kitty_hk_active = NULL;
}

static void scb_panel_hostkeys(struct controlbox *b)
{
    static const char *const path = KCFG_PATH_HOSTKEYS;
    struct hk_data *hk = (struct hk_data *)ctrl_alloc(b, sizeof(*hk));
    struct controlset *s;
    dlgcontrol *c;

    /* A previous box's run keeps its thread; its timer must not reach the
     * freed panel state. */
    if (kitty_hk_active && kitty_hk_active->run)
        hk_run_stop(kitty_hk_active);
    hk_timer_hk = NULL;
    memset(hk, 0, sizeof(*hk));
    kitty_hk_active = hk;
    kitty_cfg_box_closing_hook = hk_box_closing;
    ctrl_settitle(b, path, KT_HK_TITLE);
    s = ctrl_getset(b, path, "keys", KT_HK_GROUP);
    ctrl_text(s, KT_HK_INTRO, HELPCTX(kitty_host_keys));
    hk->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                               kitty_hk_handler, P(hk));
    hk->listbox->context2 = I(0);
    hk->listbox->listbox.height = 4;     /* the floor; the fill hook grows it */
    hk->listbox->listbox.multisel = 2;   /* extended: the arrow keys select */
    hk->listbox->listbox.headerrow = true;
    hk->listbox->listbox.rowink = hk_row_ink;
    hk->listbox->listbox.ncols = 6;
    hk->listbox->listbox.percentages = snewn(6, int);
    hk->listbox->listbox.percentages[0] = 26;   /* Host */
    hk->listbox->listbox.percentages[1] = 20;   /* Type and bits */
    hk->listbox->listbox.percentages[2] = 16;   /* SHA256 (a prefix; the detail box has it whole) */
    hk->listbox->listbox.percentages[3] = 11;   /* First seen */
    hk->listbox->listbox.percentages[4] = 11;   /* Last written */
    hk->listbox->listbox.percentages[5] = 16;   /* Verified */
    /* The selected key in full: both fingerprints, both stamps, the klink
     * call that repeats the check, and the verdict. A read-only edit, so
     * the text can be selected and copied too. */
    hk->detail = ctrl_editbox_multiline(s, NULL, NO_SHORTCUT, 6, true,
                                        HELPCTX(kitty_host_keys),
                                        kitty_hk_handler, P(hk), P(NULL));
    hk->detail->context2 = I(3);
    /* The count / progress / summary line gets the full width: the
     * verification summary did not fit beside three buttons. */
    hk->banner = ctrl_text(s, " ", HELPCTX(kitty_host_keys));
    ctrl_columns(s, 3, 34, 33, 33);
    c = ctrl_pushbutton(s, KT_HK_COPY, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hk_handler, P(hk));
    c->context2 = I(1); c->column = 0;
    c = ctrl_pushbutton(s, KT_HK_DELETE, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hk_handler, P(hk));
    c->context2 = I(2); c->column = 1;
    c = ctrl_pushbutton(s, KT_HK_VERIFY, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hk_handler, P(hk));
    c->context2 = I(4); c->column = 2;
    hk->verify = c;
    ctrl_columns(s, 1, 100);
}

/* ---- the splitter between the list and the detail box ---------------------- */

/*
 * After a Verify, the detail box is where the full
 * fingerprints and the verdict are read, and four visible lines are few. A
 * thin bar between the list and the box moves the boundary with the mouse:
 * the list gives what the box gains. The offset is remembered for the
 * process (the panel is re-laid out on every resize and every visit; the
 * placement hook puts the boundary back where it was dragged to).
 */
static int hk_split_offset = 0;             /* px the boundary was dragged down (+) or up (-) */
HWND hk_splitter = NULL;
static HWND hk_split_above = NULL, hk_split_below = NULL;   /* the list, the detail box */
static int hk_split_drag_y = -1;            /* screen y at button-down, -1 = not dragging */
static int hk_split_min_above = 60, hk_split_min_below = 40;

static bool hk_split_rects(RECT *a, RECT *b)
{
    HWND parent;
    if (!hk_split_above || !hk_split_below || !IsWindow(hk_split_above) || !IsWindow(hk_split_below))
        return false;
    parent = GetParent(hk_split_above);
    if (!GetWindowRect(hk_split_above, a) || !GetWindowRect(hk_split_below, b))
        return false;
    MapWindowPoints(NULL, parent, (POINT *)a, 2);
    MapWindowPoints(NULL, parent, (POINT *)b, 2);
    return true;
}

/* Put the boundary at anchor + dy (clamped, whole rows). `a`/`b` are the
 * rects the drag started from, in parent coordinates. Whole rows, because
 * a list box rounds its height to them anyway: asking for a row-multiple
 * means the list ends exactly where the box and the bar are put, and a
 * move that rounds to the row already shown does nothing at all - the
 * three windows move in ONE deferred pass, so a drag does not flicker
 * (the first version flickered on every drag). */
static int hk_split_shown_dy = INT_MIN;     /* the dy last applied in this drag */
static void hk_split_apply(const RECT *a, const RECT *b, int dy)
{
    int gap = b->top - a->bottom, ih;
    HDWP dwp;
    if (!hk_split_above || !hk_split_below) return;
    ih = (int)SendMessage(hk_split_above, LB_GETITEMHEIGHT, 0, 0);
    if (ih > 0) dy = (dy >= 0 ? (dy + ih / 2) / ih : -((-dy + ih / 2) / ih)) * ih;   /* whole rows */
    if ((a->bottom - a->top) + dy < hk_split_min_above) dy = hk_split_min_above - (a->bottom - a->top);
    if ((b->bottom - b->top) - dy < hk_split_min_below) dy = (b->bottom - b->top) - hk_split_min_below;
    if (ih > 0) dy = (dy / ih) * ih;                    /* the clamp must not break the row grid */
    if (dy == hk_split_shown_dy) return;                /* nothing changed: no repaint */
    hk_split_shown_dy = dy;
    dwp = BeginDeferWindowPos(3);
    if (dwp) dwp = DeferWindowPos(dwp, hk_split_above, NULL, 0, 0, a->right - a->left,
                                  (a->bottom - a->top) + dy,
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (dwp) dwp = DeferWindowPos(dwp, hk_split_below, NULL, b->left, a->bottom + dy + gap,
                                  b->right - b->left, b->bottom - (a->bottom + dy + gap),
                                  SWP_NOZORDER | SWP_NOACTIVATE);
    if (dwp && hk_splitter)
        dwp = DeferWindowPos(dwp, hk_splitter, HWND_TOP, a->left, a->bottom + dy - 2,
                             a->right - a->left, gap + 4, SWP_NOACTIVATE);
    if (dwp) EndDeferWindowPos(dwp);
}

/* the drag: anchor rects and the offset at button-down */
static RECT hk_drag_a, hk_drag_b;
static int hk_drag_off0;

static LRESULT CALLBACK hk_splitter_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZENS));
        return TRUE;
      case WM_LBUTTONDOWN: {
        POINT pt; GetCursorPos(&pt);
        if (!hk_split_rects(&hk_drag_a, &hk_drag_b)) return 0;
        hk_split_drag_y = pt.y;
        hk_drag_off0 = hk_split_offset;
        hk_split_shown_dy = INT_MIN;
        SetCapture(hwnd);
        return 0;
      }
      case WM_MOUSEMOVE:
        if (hk_split_drag_y >= 0 && (wParam & MK_LBUTTON)) {
            POINT pt; GetCursorPos(&pt);
            /* always from the anchor: the total drag, not a delta chain */
            hk_split_apply(&hk_drag_a, &hk_drag_b, pt.y - hk_split_drag_y);
            if (hk_split_shown_dy != INT_MIN)
                hk_split_offset = hk_drag_off0 + hk_split_shown_dy;
        }
        return 0;
      case WM_LBUTTONUP:
      case WM_CAPTURECHANGED:
        if (hk_split_drag_y >= 0) { hk_split_drag_y = -1; if (GetCapture() == hwnd) ReleaseCapture(); }
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        {
            HBRUSH back = kitty_theme_backbrush(GetParent(hwnd));
            FillRect(hdc, &r, back ? back : GetSysColorBrush(COLOR_BTNFACE));
        }
        /* the grip: three dots in the middle, in the text colour, dimmed */
        {
            bool dark = kitty_theme_window_dark(GetParent(hwnd));
            COLORREF ink = dark ? RGB(150, 150, 150) : GetSysColor(COLOR_GRAYTEXT);
            int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            for (int i = -1; i <= 1; i++) {
                RECT d = { cx + i * 6 - 1, cy - 1, cx + i * 6 + 1, cy + 1 };
                HBRUSH b = CreateSolidBrush(ink);
                FillRect(hdc, &d, b);
                DeleteObject(b);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* Called from the placement hook once the leaf is laid out: create or move
 * the bar into the gap between the list and the detail box, then put the
 * boundary back where it was last dragged. */
void hk_place_splitter(struct hk_data *hk)
{
    static bool registered = false;
    HWND above, below, parent;
    RECT a, b;
    int want;

    if (!hk || !hk->listbox || !hk->detail) return;
    above = kitty_cfg_ctrl_hwnd(hk->listbox);
    below = kitty_cfg_ctrl_hwnd(hk->detail);
    if (!above || !below) return;
    hk_split_above = above; hk_split_below = below;
    parent = GetParent(above);
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = hk_splitter_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "KittySplitter";
        wc.hCursor = LoadCursor(NULL, IDC_SIZENS);
        RegisterClassA(&wc);
        registered = true;
    }
    if (hk_splitter && (!IsWindow(hk_splitter) || GetParent(hk_splitter) != parent)) {
        if (IsWindow(hk_splitter)) DestroyWindow(hk_splitter);
        hk_splitter = NULL;
    }
    if (!hk_split_rects(&a, &b)) return;
    /* the list's row height bounds the drag: two rows plus the header at least */
    {
        int ih = (int)SendMessage(above, LB_GETITEMHEIGHT, 0, 0);
        if (ih > 0) hk_split_min_above = ih * 3 + 4;
        hk_split_min_below = ih > 0 ? ih * 2 + 4 : 40;
    }
    if (!hk_splitter)
        hk_splitter = CreateWindowExA(0, "KittySplitter", "", WS_CHILD | WS_VISIBLE,
                                      a.left, a.bottom - 2, a.right - a.left, (b.top - a.bottom) + 4,
                                      parent, NULL, GetModuleHandle(NULL), NULL);
    else
        SetWindowPos(hk_splitter, HWND_TOP, a.left, a.bottom - 2, a.right - a.left,
                     (b.top - a.bottom) + 4, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    /* the layout put the boundary at its natural place; re-apply the drag */
    want = hk_split_offset;
    hk_split_shown_dy = INT_MIN;
    if (want) {
        hk_split_apply(&a, &b, want);
        hk_split_offset = hk_split_shown_dy != INT_MIN ? hk_split_shown_dy : 0;
    }
}

/* ==== Updates, foreign sessions, agent check, missing features ========== */

static void checkupdate_button_handler(dlgcontrol *ctrl, dlgparam *dp,
                                       void *data, int event)
{
    if (event == EVENT_ACTION)
        CheckVersionFromWebSite(GetActiveWindow(), 0);   /* config box: no live terminal */
}

/* KiTTY: checkbox to also show (and thus allow deleting) sessions stored in the
 * read-only fallback hives (old 9bis KiTTY + stock PuTTY). Off by default so the
 * list shows only KiTTY's own sessions and a stock-PuTTY session is never
 * deleted unless the user deliberately reveals it. Toggling re-enumerates the
 * list immediately. The flag lives in the registry (windows/storage.c). */
/*
 * The line under the saved-session list, and the button beside it.
 *
 * It exists for one reason: the list is showing sessions the user did not
 * create in this KiTTY, and nothing else on the panel says so. The button is
 * the same jump the proxy panel uses - the setting lives on one panel only,
 * and this is a pointer to it, not a second copy of it.
 */
void kitty_foreignnotice_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    (void)ctrl; (void)dlg; (void)data;
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/Migration");
}

static void kitty_showforeign_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)ctrl->context.p;
    if (!ssd)
        ssd = session_filter_ssd;      /* Application > Migration */
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, kitty_get_show_foreign_sessions());
    } else if (event == EVENT_VALCHANGE) {
        kitty_set_show_foreign_sessions(dlg_checkbox_get(ctrl, dlg));
        /* re-enumerate so the list shows/hides the foreign sessions at once */
        if (ssd) {
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            kitty_session_folder_cache_clear();
            dlg_refresh(ssd->listbox, dlg);
        }
    }
}

/* KiTTY: [KiTTY] verifyagent - the warning a signed kitty.exe shows when an
 * unverified program answers its SSH agent requests (kitty_win.c, the
 * serving-agent check). A GLOBAL application setting, deliberately not part
 * of the session's Conf: the checkbox reads and writes the settings store
 * directly, and a change applies to windows opened from then on. */
static void kitty_verifyagent_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    (void)data;
    if (event == EVENT_REFRESH) {
        char cfg[16];
        int warn = 1;
        if (ReadParameterN(INIT_SECTION, KI_VERIFYAGENT, cfg, sizeof(cfg)) &&
            !stricmp(cfg, "no"))
            warn = 0;
        dlg_checkbox_set(ctrl, dlg, warn);
    } else if (event == EVENT_VALCHANGE) {
        WriteParameter(INIT_SECTION, KI_VERIFYAGENT,
                       dlg_checkbox_get(ctrl, dlg) ? "yes" : "no");
    }
}

/* KiTTY: [KiTTY] warnmissingfeatures - the one line a session prints naming
 * what this version of Windows cannot provide (kitty_win.c,
 * kitty_report_missing_features). Global, like the switch above, and read
 * when a window opens: a change reaches the windows opened after it. The
 * Event Log entry is written whatever this says - the checkbox governs the
 * interruption, not the record. */
static void kitty_warnfeatures_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    (void)data;
    if (event == EVENT_REFRESH) {
        char cfg[16];
        int say = 1;
        if (ReadParameterN(INIT_SECTION, KI_WARNMISSINGFEATURES, cfg,
                           sizeof(cfg)) && !stricmp(cfg, "no"))
            say = 0;
        dlg_checkbox_set(ctrl, dlg, say);
    } else if (event == EVENT_VALCHANGE) {
        WriteParameter(INIT_SECTION, KI_WARNMISSINGFEATURES,
                       dlg_checkbox_get(ctrl, dlg) ? "yes" : "no");
    }
}


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


/*
 * The program name the six panel titles print ("Basic options for your %s
 * session", "Options controlling %s's window", ...). KiTTY's own panels name
 * KiTTY++; the stock PuTTY build and -putty mode keep `appname`, which is
 * what those builds are called.
 */
const char *scb_title_appname(void)
{
    return GetPuttyFlag() ? appname : KT_CAP_KITTYPP;
}


/* ==== Config Window: theme, flags, sizes - and Security ================= */

/*
 * Application > Config Window.
 *
 * Settings about the configuration box itself. All three are kitty.ini keys,
 * not session values, so the handlers read and write there directly - there is
 * no Save on an application setting and no Conf that could carry them.
 *
 * WARNING: NONE of them can take effect in the window you are looking at. The box's
 * geometry is decided when it is built, and the theme is applied to windows as
 * they are created; changing either here writes the file and the next
 * configuration window comes up with it. The panel says so rather than leaving
 * someone to wonder why nothing moved.
 */
static void kitty_cfgwin_theme_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    static const int prefs[] = { KITTY_THEME_SYSTEM, KITTY_THEME_LIGHT,
                                 KITTY_THEME_DARK };
    static const char *const names[] = { KT_CFG_THEME_FOLLOW_WINDOWS, KT_CFG_THEME_LIGHT, KT_CFG_THEME_DARK };
    int i;

    if (event == EVENT_REFRESH) {
        int cur = kitty_theme_app_pref();
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < 3; i++)
            dlg_listbox_addwithid(ctrl, dlg, names[i], prefs[i]);
        for (i = 0; i < 3; i++)
            if (prefs[i] == cur)
                dlg_listbox_select(ctrl, dlg, i);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx >= 0 && idx < 3) {
            WriteParameter(INIT_SECTION, KI_THEME,
                           (char *)kitty_theme_pref_to_string(prefs[idx]));
            kitty_theme_app_pref_forget();   /* the cached answer is stale */
            /*
             * And show it, here, now.
             *
             * The colours DID change without a restart - on the next
             * activation, because the CBT hook re-themes a window whose
             * remembered darkness no longer matches. So the setting looked
             * inert until you clicked away and back. This is that same call,
             * made at the moment the choice is made; the theme reads the file
             * we have just written. Other open windows follow when they are
             * next activated, which is the behaviour that was already there.
             */
            kitty_theme_apply(kitty_cfg_modal_owner(), kitty_theme_app_dark());
            /* ...and the terminal's own title bar and border, through the
             * resting-state path (a workplace green stays green). In the
             * hand-off process there is no terminal and the call is a no-op. */
            kitty_frame_restore_resting();
            /* ...and the popup menus of this process, from the next one
             * opened (the app mode, then FlushMenuThemes). */
            kitty_theme_app_mode(kitty_theme_app_pref());
        }
    }
}

/*
 * The Session-panel group on Application > Config Window.
 *
 * These settings were all kitty.ini-only until now, and they share a shape:
 * each decides what the configuration window PUTS IN the Session panel, and
 * each is read when the panel's controls are declared - so a change lands in
 * the next configuration window, not this one. Said once at the foot of the
 * group rather than on every control.
 *
 * ctrl->context.p names the key, exactly as the number fields above do.
 */
/* kitty.c owns these; declared here because this file has no header for them
 * and an implicit declaration disagrees with the const-qualified real one. */

static void kitty_cfgwin_flag_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    const char *key = (const char *)ctrl->context.p;
    int cur;

    if (!strcmp(key, KI_CONFIGBOX_FILTER))              cur = GetSessionFilterFlag();
    else if (!strcmp(key, KI_CONFIGBOX_DEFAULTSETTINGS)) cur = GetDefaultSettingsFlag();
    else if (!strcmp(key, KI_CONFIGBOX_FOLDERNAVIGATION)) cur = GetFolderNavigationFlag();
    else                                      cur = GetLoadLastSessionFlag();

    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, cur != 0);
    } else if (event == EVENT_VALCHANGE) {
        int on = dlg_checkbox_get(ctrl, dlg) ? 1 : 0;
        WriteParameter(KI_SECTION_CONFIGBOX, (char *)key, on ? "yes" : "no");
        /* The running value too - EVENT_REFRESH answers from it, so writing
         * only the file leaves the box redisplaying the old state the moment
         * the panel is left and re-entered. */
        if (!strcmp(key, KI_CONFIGBOX_FILTER))               SetSessionFilterFlag(on);
        else if (!strcmp(key, KI_CONFIGBOX_DEFAULTSETTINGS)) SetDefaultSettingsFlag(on);
        else if (!strcmp(key, KI_CONFIGBOX_FOLDERNAVIGATION)) SetFolderNavigationFlag(on);
        else                                       SetLoadLastSessionFlag(on);
    }
}

/* Two droplists in the same group: the named-proxy chooser's visibility, and
 * what a double click on a saved session does. */
static void kitty_cfgwin_proxysel_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                          void *data, int event)
{
    static const int vals[] = { 0, 1, -1 };            /* auto, yes, no */
    static const char *const names[] = {
        KT_CFG_PROXYCHOOSER_ONCE_DEFINED, KT_SESSION_ALWAYS, KT_SESSION_NEVER };
    static const char *const keys[] = { "auto", "yes", "no" };
    int i;

    if (event == EVENT_REFRESH) {
        int cur = GetProxySelectionFlag();
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < 3; i++)
            dlg_listbox_addwithid(ctrl, dlg, names[i], vals[i]);
        for (i = 0; i < 3; i++)
            if (vals[i] == cur)
                dlg_listbox_select(ctrl, dlg, i);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx >= 0 && idx < 3) {
            WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_PROXYSELECTION, (char *)keys[idx]);
            SetProxySelectionFlag(vals[idx]);
        }
    }
}

/* How deep the category tree comes up expanded. Stored as "all" or a depth. */
static void kitty_cfgwin_expand_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    static const char *const names[] = {
        KT_CFG_TREE_EVERYTHING, KT_CFG_TREE_TOP_ONLY, KT_CFG_TREE_TWO_LEVELS, KT_CFG_TREE_THREE_LEVELS };
    static const char *const keys[] = { "all", "1", "2", "3" };
    static const int depths[] = { 99, 1, 2, 3 };
    int i;

    if (event == EVENT_REFRESH) {
        int cur = kitty_category_expand_depth;
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < 4; i++)
            dlg_listbox_addwithid(ctrl, dlg, names[i], depths[i]);
        /* Anything deeper than the offered list is "everything" as far as
         * this box is concerned - it is what the user sees. */
        dlg_listbox_select(ctrl, dlg, 0);
        for (i = 1; i < 4; i++)
            if (depths[i] == cur)
                dlg_listbox_select(ctrl, dlg, i);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx >= 0 && idx < 4) {
            WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_CATEGORYEXPAND, (char *)keys[idx]);
            kitty_category_expand_depth = depths[idx];
        }
    }
}

/* Respawning the picker when a connected window closes. */
static void kitty_cfgwin_noexit_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{

    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, GetConfigBoxNoExitFlag() != 0);
    } else if (event == EVENT_VALCHANGE) {
        int on = dlg_checkbox_get(ctrl, dlg) ? 1 : 0;
        WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_NOEXIT, on ? "yes" : "no");
        SetConfigBoxNoExitFlag(on);
    }
}

static void kitty_cfgwin_fixedsize_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                           void *data, int event)
{

    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, GetConfigBoxFixedSizeFlag() != 0);
    } else if (event == EVENT_VALCHANGE) {
        int on = dlg_checkbox_get(ctrl, dlg) ? 1 : 0;
        WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_FIXEDSIZEWINDOW, on ? "yes" : "no");
        SetConfigBoxFixedSizeFlag(on);
        kitty_cfgbox_apply_fixed_size();
    }
}

static void kitty_cfgwin_dblclick_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                          void *data, int event)
{
    static const char *const names[] = { KT_CFG_DBLCLICK_OPEN_CLOSE,
                                         KT_CFG_DBLCLICK_START_NEW };
    static const char *const keys[] = { "open", "start" };
    int i;

    if (event == EVENT_REFRESH) {
        int cur = GetDblClickFlag() ? 1 : 0;
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < 2; i++)
            dlg_listbox_addwithid(ctrl, dlg, names[i], i);
        dlg_listbox_select(ctrl, dlg, cur);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx >= 0 && idx < 2) {
            WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_DBLCLICK, (char *)keys[idx]);
            SetDblClickFlag(idx);
        }
    }
}

static void kitty_cfgwin_num_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    const char *key = (const char *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        extern int GetConfigBoxHeight(void);        /* kitty.c: rows */
        extern int GetConfigBoxWindowHeight(void);  /* kitty.c: pixels, 0 = fit */
        extern int GetConfigBoxWindowWidth(void);   /* kitty.c: pixels, 0 = fit */
        char buf[32];
        int v = !strcmp(key, KI_CONFIGBOX_HEIGHT)      ? GetConfigBoxHeight()
              : !strcmp(key, KI_CONFIGBOX_WINDOWWIDTH) ? GetConfigBoxWindowWidth()
                                            : GetConfigBoxWindowHeight();
        buf[0] = '\0';
        if (v > 0)
            sprintf(buf, "%d", v);
        /*
         * dlg_editbox_set fires EVENT_VALCHANGE, and there is no re-entry
         * guard inside it. Unguarded, every refresh of this panel wrote the
         * number straight back - which would PIN the default into kitty.ini
         * for someone who had never set it, purely because they opened the
         * panel and looked at it.
         */
        cfgwin_refreshing = 1;
        dlg_editbox_set(ctrl, dlg, buf);
        cfgwin_refreshing = 0;
    } else if (event == EVENT_VALCHANGE && !cfgwin_refreshing) {
        /* Written as it is typed, like every other application setting here:
         * there is no Save on this panel. Half-typed numbers reach the file
         * and are immediately replaced by the next keystroke - harmless,
         * because neither key does anything until the next window is built. */
        char *s;
        /* "Lock window size": the two window fields refuse edits - the typed
         * text is put back to the stored value at once. This box has no way
         * to grey a control, so refusing is how read-only is shown. */
        if (strcmp(key, KI_CONFIGBOX_HEIGHT) != 0 && kitty_cfgbox_size_locked()) {
            kitty_cfgwin_num_handler(ctrl, dlg, data, EVENT_REFRESH);
            return;
        }
        s = dlg_editbox_get(ctrl, dlg);
        if (s[0]) {
            /*
             * Store what will actually be USED, not what was typed.
             *
             * A number the box then clamps is not the setting: leaving it in
             * the file means the field redisplays a size the window never
             * had, and the user is left looking for the reason nothing
             * happened. The row count has KITTY_CFG_SESSION_ROWS_MIN as its
             * floor (kitty_defs.h, shared with kitty_config_session_rows and
             * the label text); the window sizes have
             * the box's own minimum, which only dialog.c knows, so it reports
             * back what it applied.
             */
            char applied[32];
            const char *store = s;         /* NEVER reassign s - it is freed */
            if (!strcmp(key, KI_CONFIGBOX_HEIGHT)) {
                int v = atoi(s);
                if (v < KITTY_CFG_SESSION_ROWS_MIN)
                    v = KITTY_CFG_SESSION_ROWS_MIN;
                sprintf(applied, "%d", v);
                store = applied;
            }
            WriteParameter(KI_SECTION_CONFIGBOX, (char *)key, (char *)store);
            /*
             * And into the RUNNING program, not only the file.
             *
             * EVENT_REFRESH above answers from these two, because they are
             * what the box is actually built from. Writing the file alone
             * left them disagreeing: the number was stored, the next window
             * came up the new size - and this panel snapped back to the old
             * value as soon as it was left and re-entered, which reads as the
             * field refusing to take the change.
             */
            if (!strcmp(key, KI_CONFIGBOX_HEIGHT))
                SetConfigBoxHeight(atoi(store));   /* the clamped one */
            else if (!strcmp(key, KI_CONFIGBOX_WINDOWWIDTH))
                SetConfigBoxWindowWidth(atoi(s));
            else
                SetConfigBoxWindowHeight(atoi(s));
            if (!strcmp(key, KI_CONFIGBOX_HEIGHT)) {
                /*
                 * Live, now that the button column is placed from the list's
                 * measured rectangle instead of being spaced by a computed
                 * number of blank rows: the row count no longer decides how
                 * many CONTROLS the set holds, so it is a property the panel
                 * can simply be laid out again with.
                 *
                 * Two steps, and both are needed - the control carries the
                 * height, and the panel carries the control.
                 */
                if (kitty_session_ssd && kitty_session_ssd->listbox)
                    kitty_session_ssd->listbox->listbox.height =
                        kitty_config_session_rows();
                kitty_cfgbox_relayout_panel("Session");
            } else {
                kitty_cfgbox_apply_size();
            }
        }
        sfree(s);
    }
}

static void scb_panel_config_window(struct controlbox *b, bool midsession)
{
    struct controlset *s;

    if (midsession || GetPuttyFlag())
        return;

    ctrl_settitle(b, "Application/Config Window",
                  KT_CONFIG_WINDOW_THIS_WINDOW);

    /* The colour theme is NOT here any more: it is one setting for the whole
     * suite, and sits on KiTTY++ Settings > Appearance. */
    s = ctrl_getset(b, "Application/Config Window", "look", KT_CONFIG_WINDOW_CATEGORY_TREE);
    /* The percentage is the LIST's share of the line: 30 leaves the label
     * its room and makes the list as narrow as its two short entries allow. */
    ctrl_droplist(s, KT_CONFIG_WINDOW_CATEGORY_TREE_OPENS_SHOWING, NO_SHORTCUT, 30,
                  HELPCTX(kitty_theme), kitty_cfgwin_expand_handler, P(NULL));

    s = ctrl_getset(b, "Application/Config Window", "size", KT_CONFIG_WINDOW_SIZE);
    /* PIXELS. dialog.c multiplies these by the DPI scale and gives the window
     * that size; they are not dialog units, whatever the old label said.
     * Both labels say the same short thing: the width's was long enough to
     * run under its own edit box at this font. */
    ctrl_editbox(s, KT_CONFIG_WINDOW_WINDOW_HEIGHT_IN_PIXELS_BLANK,
                 NO_SHORTCUT, 30, HELPCTX(kitty_theme),
                 kitty_cfgwin_num_handler, P(KI_CONFIGBOX_WINDOWHEIGHT), ED_STR);
    ctrl_editbox(s, KT_CONFIG_WINDOW_WINDOW_WIDTH_IN_PIXELS_BLANK,
                 NO_SHORTCUT, 30, HELPCTX(kitty_theme),
                 kitty_cfgwin_num_handler, P(KI_CONFIGBOX_WINDOWWIDTH), ED_STR);
    ctrl_checkbox(s, KT_CONFIG_WINDOW_LOCK_WINDOW_SIZE, NO_SHORTCUT,
                  HELPCTX(kitty_theme), kitty_cfgwin_fixedsize_handler,
                  P(NULL));

    s = ctrl_getset(b, "Application/Config Window", "closing",
                    KT_CONFIG_WINDOW_CLOSING_A_TERMINAL_WINDOW);
    ctrl_checkbox(s, KT_CONFIG_WINDOW_COME_BACK_TO_THIS_WINDOW,
                  NO_SHORTCUT, HELPCTX(kitty_theme),
                  kitty_cfgwin_noexit_handler, P(NULL));
}

/*
 * Application > Security, and its Certification authorities leaf.
 *
 * Both hold things that are true of the INSTALLATION rather than of one
 * connection: whether an unverified agent is worth warning about, and which
 * host CAs this machine trusts. The CA records were always store-wide - see
 * enum_host_ca_start() - but the only way to reach them was a pop-up launched
 * from a session's SSH panel, which read as though they belonged to that
 * session.
 *
 * The CA leaf builds the SAME controls as that pop-up, from
 * setup_ca_config_box_at() in ssh/ca-config.c, without its Done button: the
 * configuration box has buttons of its own.
 */
static void scb_panel_security(struct controlbox *b, bool midsession)
{
    struct controlset *s;

    if (midsession || GetPuttyFlag())
        return;

    ctrl_settitle(b, "Application/Security", KT_SECURITY_SECURITY);
    s = ctrl_getset(b, "Application/Security", "agent", KT_SECURITY_SSH_AGENT);
    ctrl_checkbox(s, KT_SECURITY_WARN_WHEN_AN_UNVERIFIED_AGENT,
                  NO_SHORTCUT, HELPCTX(kitty_verifyagent),
                  kitty_verifyagent_handler, P(NULL));

    /* What this Windows is too old to provide. On the Security panel because
     * most of what goes missing on an old system protects something - Windows
     * Hello, encrypted memory for secrets - and because someone who has read
     * the line once needs somewhere to turn it off. */
    s = ctrl_getset(b, "Application/Security", "thiswindows",
                    KT_SECURITY_WINDOWS_SUPPORTED_FEATURES);
    ctrl_checkbox(s, KT_SECURITY_NOTIFY_UNSUPPORTED_LIBS,
                  NO_SHORTCUT, HELPCTX(kitty_missing_features),
                  kitty_warnfeatures_handler, P(NULL));
    ctrl_text(s, KT_SECURITY_MISSING_CAN_LIMIT, HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_HELLO,  HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_DARK,   HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_DPI,    HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_MEMENC, HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_SSO,    HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_LIMIT_IPV6,   HELPCTX(kitty_missing_features));
    ctrl_text(s, KT_SECURITY_EVENTLOG_ALWAYS, HELPCTX(kitty_missing_features));

    /* The tracing switch ([KiTTY] debug) was on Automation, but what it
     * traces - session lookups, the automatic command, key remaps, helper
     * command lines - is not the automation's alone, so its group is here,
     * at the bottom of the panel. Same KiTTY++ Settings machinery
     * (kitty_kset_handler over the kset_keys table), only the panel moved. */
    s = ctrl_getset(b, "Application/Security", "diag", KT_SECURITY_DIAGNOSTICS);
    ctrl_checkbox(s, KT_KSET_TW_DEBUG, NO_SHORTCUT, HELPCTX(kitty_verifyagent),
                  kitty_kset_handler, P((void *)kset_find(KI_DEBUG)));

    if (has_ca_config_box) {
        ctrl_settitle(b, "Application/Security/Certificate Authorities",
                      KT_CERTIFICATE_AUTHORITIES_TRUSTED_HOST_CERTIFICATE_AUTHORITIES);
        /*
         * Say what the panel is FOR before showing its controls.
         *
         * Upstream's version is a pop-up reached from a session's Host keys
         * panel, where the surrounding context supplies the meaning. Standing
         * on its own it offers a name, a key and a host expression with no
         * hint of where any of them come from.
         */
        s = ctrl_getset(b, "Application/Security/Certificate Authorities",
                        "intro", NULL);
        ctrl_text(s, KT_CERTIFICATE_AUTHORITIES_A_CERTIFICATE_AUTHORITY_SIGNS_HOST, HELPCTX(kitty_host_cas));
        setup_ca_config_box_at(
            b, "Application/Security/Certificate Authorities", false);
        /* No trailing note about the Valid-hosts expression: dangling under
         * the whole editor it explained nothing in particular, and the help
         * carries the expression syntax in full. */
    }

    /* Security > Interactive Login: what happens to a login you TYPE. After a
     * login the name and password go into the running session's settings
     * (SetUsernameInConfig / SetPasswordInConfig in kitty.c), which a
     * duplicate inherits and a mid-session Save writes out. The switch
     * stops that; a password stored on the Login panel on purpose is not
     * touched by it. */
    ctrl_settitle(b, "Application/Security/Interactive Login", KT_PASSWORDS_TITLE);
    s = ctrl_getset(b, "Application/Security/Interactive Login", "typed", KT_PASSWORDS_TYPED);
    /* The settings tree's handler and table (declared above scb_panel_zmodem,
     * defined with the KiTTY++ Settings leaves further down). */
    ctrl_checkbox(s, KT_KSET_CN_NOSAVE, NO_SHORTCUT, HELPCTX(kitty_passwords),
                  kitty_kset_handler, P((void *)kset_find(KI_USERPASSSSHNOSAVE)));
    ctrl_text(s, KT_PASSWORDS_TYPED_DEFAULT, HELPCTX(kitty_passwords));
    ctrl_text(s, KT_PASSWORDS_TYPED_CONSEQUENCE, HELPCTX(kitty_passwords));

    /* Security > Client Identity: what KiTTY tells an SSH server it is. The
     * whole banner follows the field as it is typed - the field edits one
     * token, and only the full string says what that token does. */
    ctrl_settitle(b, "Application/Security/Client Identity", KT_CLIENT_IDENTITY_TITLE);
    s = ctrl_getset(b, "Application/Security/Client Identity", "banner", KT_CLIENT_IDENTITY_BANNER);
    ctrl_editbox(s, KT_KSET_CN_SSHVERSION, NO_SHORTCUT, 100, HELPCTX(kitty_client_identity),
                 kitty_kset_handler, P((void *)kset_find(KI_SSHVERSION)), ED_STR);
    kset_sshver_preview = ctrl_text(s, " ", HELPCTX(kitty_client_identity));
    ctrl_text(s, KT_KSET_CN_SSHVERSION_NOTE, HELPCTX(kitty_client_identity));

    /* Security > Clipboard: the large-paste guard. One global key
     * ([KiTTY] pastesize) - there is no per-session form of it. */
    ctrl_settitle(b, "Application/Security/Clipboard", KT_CLIPBOARD_TITLE);
    s = ctrl_getset(b, "Application/Security/Clipboard", "paste", KT_CLIPBOARD_PASTE);
    {
        /* label + field, then the unit after the field on the same row */
        dlgcontrol *pc;
        ctrl_columns(s, 2, 72, 28);
        pc = ctrl_editbox(s, KT_KSET_TW_PASTESIZE, NO_SHORTCUT, 30, HELPCTX(kitty_clipboard),
                          kitty_kset_handler, P((void *)kset_find(KI_PASTESIZE)), ED_STR);
        pc->column = 0;
        pc = ctrl_text(s, KT_KSET_TW_PASTESIZE_UNIT, HELPCTX(kitty_clipboard));
        pc->column = 1;
        ctrl_columns(s, 1, 100);
    }
    ctrl_text(s, KT_CLIPBOARD_PASTE_SCOPE, HELPCTX(kitty_clipboard));

    /* Security > Application Notification: one note for the whole
     * installation, shown by the first window of every KiTTY++ process
     * (kitty/kitty_notes.c). Stored as one escaped line in [KiTTY] notes,
     * hence KSET_MULTITEXT rather than KSET_TEXT. */
    ctrl_settitle(b, "Application/Security/Application Notification",
                  KT_APPNOTIFICATION_TITLE);
    s = ctrl_getset(b, "Application/Security/Application Notification",
                    "note", NULL);
    ctrl_editbox_multiline(s, KT_APPNOTIFICATION_FIELD, NO_SHORTCUT, 6, false,
                           HELPCTX(kitty_application_notification),
                           kitty_kset_handler,
                           P((void *)kset_find(KI_NOTES)), ED_STR);
    ctrl_text(s, KT_APPNOTIFICATION_NOTE,
              HELPCTX(kitty_application_notification));
    /* Off by default: the note is meant to be shown at every start, and
     * silencing it is the deliberate choice. */
    ctrl_checkbox(s, KT_APPNOTIFICATION_ONCE, NO_SHORTCUT,
                  HELPCTX(kitty_application_notification),
                  kitty_kset_handler, P((void *)kset_find(KI_NOTESONCE)));

    /* Security > Host keys: the trust store, listed (kitty_hostkeys.c). */
    scb_panel_hostkeys(b);
    /* Security > Applications: the program files beside this one and
     * whether they check out (kitty_config_apps.c). */
    scb_panel_applications(b);
}

/* A control per kind, so the leaf builders read as a list of settings. */
#define KSET_CHECKBOX(s, label, key, hc) \
    ctrl_checkbox(s, label, NO_SHORTCUT, HELPCTX(hc), kitty_kset_handler, KSET(key))
#define KSET_NUMBER(s, label, key, hc) \
    ctrl_editbox(s, label, NO_SHORTCUT, 25, HELPCTX(hc), kitty_kset_handler, KSET(key), ED_STR)
#define KSET_TEXTBOX(s, label, key, hc) \
    ctrl_editbox(s, label, NO_SHORTCUT, 100, HELPCTX(hc), kitty_kset_handler, KSET(key), ED_STR)
#define KSET_DROPLIST(s, label, key, hc) \
    ctrl_droplist(s, label, NO_SHORTCUT, 55, HELPCTX(hc), kitty_kset_handler, KSET(key))
#define KSET_FILESEL(s, label, title, key, hc) \
    ctrl_filesel(s, label, NO_SHORTCUT, FILTER_ALL_FILES, false, title, HELPCTX(hc), kitty_kset_handler, KSET(key))

/* Where the whole subtree lives. */

/* KiTTY++ Settings > Appearance > Shared window position: the entry that
 * windows without a session of their own (an unnamed session, a "Default
 * Settings" window) write and that a session without an entry of its own
 * reads once (kitty_winpos.c). Two read-only lines - this monitor layout's
 * entry, and how many layouts hold one - rebuilt after Reset, which removes
 * the shared entries for every layout and nothing else. */
static dlgcontrol *ksharedpos_lines[2];

/* ==== System: the shared window position and the system paths =========== */

static void ksharedpos_text(char lines[2][256])
{
    struct kitty_termpos pos;
    int have = kitty_winpos_shared_get(kitty_winpos_layout_hash(), &pos);
    if (have && pos.cols > 0 && pos.rows > 0)
        snprintf(lines[0], 256, KT_KSET_WD_SHAREDPOS_THIS,
                 pos.left, pos.top, pos.cols, pos.rows);
    else if (have)
        snprintf(lines[0], 256, KT_KSET_WD_SHAREDPOS_THIS_POS_ONLY,
                 pos.left, pos.top);
    else
        snprintf(lines[0], 256, "%s", KT_KSET_WD_SHAREDPOS_NONE);
    snprintf(lines[1], 256, KT_KSET_WD_SHAREDPOS_COUNT,
             kitty_winpos_shared_count());
}
static void ksharedpos_refresh(dlgparam *dlg)
{
    char lines[2][256];
    int i;
    ksharedpos_text(lines);
    for (i = 0; i < 2; i++)
        if (ksharedpos_lines[i])
            dlg_label_change(ksharedpos_lines[i], dlg, lines[i]);
}
static void kitty_sharedpos_reset_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                          void *data, int event)
{
    (void)ctrl; (void)data;
    if (event != EVENT_ACTION)
        return;
    if (!kitty_confirm_box(GetActiveWindow(), KT_CAP_SHAREDPOS_RESET,
                           KT_CFG_SHAREDPOS_RESET_Q, NULL))
        return;
    kitty_winpos_shared_reset();
    ksharedpos_refresh(dlg);
}

/* KiTTY++ Settings > System: what Windows hands to this program, and the
 * buttons that register it. The five lines are rebuilt after every click. */
static dlgcontrol *ksys_lines[5];
static void ksys_refresh(dlgparam *dlg)
{
    char lines[5][256];
    int i;
    kitty_shell_integration_state(lines);
    for (i = 0; i < 5; i++)
        if (ksys_lines[i])
            dlg_label_change(ksys_lines[i], dlg, lines[i]);
}
static void kitty_system_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    const char *q;
    (void)data;
    if (event != EVENT_ACTION)
        return;
    /* Every button asks first: the first cut registered on a bare click. */
    q = ctrl->context.i == 2 ? KT_SYSTEM_UNREGISTER_Q :
        ctrl->context.i == 1 ? KT_SYSTEM_TAKEOVER_Q : KT_SYSTEM_REGISTER_Q;
    if (MessageBoxA(GetActiveWindow(), q, KT_SYSTEM_TITLE,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (ctrl->context.i == 2)
        kitty_shell_integration_unregister();
    else
        kitty_shell_integration_register(ctrl->context.i == 1);
    ksys_refresh(dlg);
}

/* System > "Add this KiTTY++ folder to the user PATH" (kitty_userpath.c) */
static void kitty_syspath_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, kitty_userpath_contains_exe_dir());
    } else if (event == EVENT_VALCHANGE) {
        char *err = NULL;
        if (!kitty_userpath_set_exe_dir(dlg_checkbox_get(ctrl, dlg), &err)) {
            char *msg = dupprintf(KT_SYSTEM_PATH_FAIL, err);
            MessageBoxA(kitty_cfg_modal_owner(), msg, KT_SYSTEM_TITLE, MB_OK | MB_ICONERROR);
            sfree(msg); sfree(err);
        }
        dlg_checkbox_set(ctrl, dlg, kitty_userpath_contains_exe_dir());
    }
}

/* ======================================================================
 * KiTTY++ Settings > Automation > Broadcast
 *
 * Three things on one leaf: the installation's master switch (sendcmdmode),
 * the installation's group key (sendcmdgroup, editable here - the panel used
 * to only SHOW it), and a send console: which sessions accept broadcasts,
 * grouped by the key they listen for, and a box of commands sent to the
 * selected groups one line at a time.
 *
 * The KiTTY++ Settings tree exists only in the start-up configuration window
 * (scb_panel_kitty_settings returns mid-session), so this leaf never sits on
 * a terminal of its own. The send still goes through the include-self form
 * of the sender, so the design's decision "the console reaches the terminal
 * it was opened from too" holds if that ever changes.
 * ====================================================================== */


/* One row of either view: a session name and the key it listens for. */
struct kbc_entry {
    char *name;
    char *key;
};

/* The leaf's state. File-scope, not ctrl_alloc'd: the send runs on a timer
 * that may outlive a hasty close of the box, and a timer callback must never
 * dereference storage the controlbox freed. Only the start-up window builds
 * this leaf, so one instance is enough. */
static struct kbc_state {
    dlgcontrol *keybox, *keyprov;
    bool setting;                  /* writing the key box ourselves */
    dlgcontrol *show;              /* the view droplist */
    dlgcontrol *list;              /* sessions, grouped under their key */
    dlgcontrol *groups;            /* the keys, multi-select */
    dlgcontrol *cmds;              /* the commands, one per line */
    dlgcontrol *load, *send, *status, *note;
    int live;                      /* 1 = open terminals, 0 = saved sessions */
    char **keys; int nkeys;        /* distinct keys of the current view */
    /* the send in progress */
    char **lines; int nlines, at;
    char **sendkeys; int nsendkeys;
    char saved_override[80];       /* -sendcmdkey as it was before the send */
    UINT_PTR timer;
    dlgparam *dlg;
    HWND dlghwnd;                  /* checked with IsWindow before dlg is used */
} kbc;

static void kbc_send_stop(bool restore_status);

/* ---- the group key ------------------------------------------------------ */

static void kbc_key_set_box(dlgparam *dlg, const char *text)
{
    kbc.setting = true;
    dlg_editbox_set(kbc.keybox, dlg, text);
    kbc.setting = false;
}

static void kbc_key_update_prov(dlgparam *dlg)
{
    if (!kbc.keyprov) return;
    dlg_label_change(kbc.keyprov, dlg,
                     kitty_broadcast_group_from_ini() ? KT_KSET_BC_KEY_CUSTOM
                                                      : KT_KSET_BC_KEY_DERIVED);
}

static void kbc_key_box_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    const struct kset_key *k = kset_find(KI_SENDCMDGROUP);
    if (event == EVENT_REFRESH) {
        /* The key IN EFFECT: the stored one, or the derived one when nothing
         * is stored - never an empty box. */
        kbc_key_set_box(dlg, kitty_broadcast_group());
        kbc_key_update_prov(dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *typed;
        if (kbc.setting)
            return;                /* our own text echoed back */
        typed = dlg_editbox_get(ctrl, dlg);
        if (!k) { sfree(typed); return; }
        if (typed && *typed) {
            /* Held back until the typing stops, like the other text fields;
             * the running value follows at once so the line below is right. */
            kset_defer_write(k, typed);
            kitty_broadcast_set_group(typed);
        } else {
            /* Emptied by hand = back to the derived key, written now so the
             * next kitty_broadcast_group() does not read the stale store. */
            kset_write(k, "");
            kitty_broadcast_set_group("");
        }
        sfree(typed);
        kbc_key_update_prov(dlg);
    }
}

static void kbc_key_copy_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    if (event != EVENT_ACTION) return;
    SetTextToClipboard(kitty_broadcast_group());
}

/* Clear = the derived key again: the store forgets sendcmdgroup, the cache is
 * dropped, the box shows what is now in effect. */
static void kbc_key_clear_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    const struct kset_key *k = kset_find(KI_SENDCMDGROUP);
    if (event != EVENT_ACTION) return;
    if (k) kset_write(k, "");
    kitty_broadcast_set_group("");
    kbc_key_set_box(dlg, kitty_broadcast_group());
    kbc_key_update_prov(dlg);
}

/* ---- the two views ------------------------------------------------------ */

static void kbc_entries_free(struct kbc_entry *e, int n)
{
    int i;
    for (i = 0; i < n; i++) { sfree(e[i].name); sfree(e[i].key); }
    sfree(e);
}

static void kbc_entries_add(struct kbc_entry **e, int *n, int *cap,
                            const char *name, const char *key)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *e = sresize(*e, *cap, struct kbc_entry);
    }
    (*e)[*n].name = dupstr(name ? name : "");
    (*e)[*n].key = dupstr(key && *key ? key : kitty_broadcast_group());
    (*n)++;
}

/* The saved sessions that accept broadcasts, from the ACTIVE store (registry
 * or the portable session files - the storage API hides which), with the key
 * each listens for: its own, or this installation's. */
static int kbc_scan_saved(struct kbc_entry **out)
{
    struct kbc_entry *e = NULL;
    int n = 0, cap = 0;
    settings_e *en = enum_settings_start();
    strbuf *name = strbuf_new();
    *out = NULL;
    if (!en) { strbuf_free(name); return 0; }
    while (enum_settings_next(en, name)) {
        settings_r *h = open_settings_r(name->s);
        if (h) {
            if (read_setting_i(h, "AcceptBroadcast", 0)) {
                char *k = read_setting_s(h, "BroadcastKey");
                kbc_entries_add(&e, &n, &cap, name->s, k);
                sfree(k);
            }
            close_settings_r(h);
        }
        strbuf_clear(name);
    }
    enum_settings_finish(en);
    strbuf_free(name);
    *out = e;
    return n;
}

/* The live view asks every terminal window "would you type a broadcast right
 * now?" (WM_COPYDATA dwData 3, windows/window.c). A window that would
 * answers with a WM_COPYDATA of its own, dwData 4, "<key>\0<session name>",
 * sent to the message-only window below while the asker is still inside its
 * SendMessage; a window that would not stays silent, exactly as it would for
 * the broadcast itself. So the list is what a send reaches, not what is
 * configured somewhere. */
static struct {
    struct kbc_entry *e;
    int n, cap;
} kbc_probe;

static LRESULT CALLBACK kbc_probe_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COPYDATA) {
        PCOPYDATASTRUCT c = (PCOPYDATASTRUCT)l;
        if (c && c->dwData == 4 && c->cbData > 0 && c->lpData) {
            /* Bounded copy: it comes from another process. */
            size_t n = c->cbData;
            char *buf, *name;
            if (n > 4096) n = 4096;
            buf = snewn(n + 2, char);
            memcpy(buf, c->lpData, n);
            buf[n] = '\0'; buf[n + 1] = '\0';
            name = buf + strlen(buf) + 1;
            if (name > buf + n) name = buf + n;
            if (!*name) {
                /* An unnamed session: the window title stands in. */
                char title[256];
                title[0] = '\0';
                if (w && IsWindow((HWND)w))
                    GetWindowTextA((HWND)w, title, sizeof(title));
                kbc_entries_add(&kbc_probe.e, &kbc_probe.n, &kbc_probe.cap, title, buf);
            } else {
                kbc_entries_add(&kbc_probe.e, &kbc_probe.n, &kbc_probe.cap, name, buf);
            }
            sfree(buf);
            return 1;
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static BOOL CALLBACK kbc_probe_enum(HWND hwnd, LPARAM lp)
{
    char cls[256];
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (!strcmp(cls, KiTTYClassName)) {
        COPYDATASTRUCT d;
        DWORD_PTR res = 0;
        d.dwData = 3;
        d.cbData = 1;                  /* an empty string: the question needs no text */
        d.lpData = (void *)"";
        /* NOT SMTO_BLOCK: the answer arrives as a sent message to this thread
         * while it waits here, and SMTO_BLOCK would refuse to take it. */
        SendMessageTimeoutA(hwnd, WM_COPYDATA, (WPARAM)(HWND)lp, (LPARAM)&d,
                            SMTO_ABORTIFHUNG, 1000, &res);
    }
    return TRUE;
}

static int kbc_scan_live(struct kbc_entry **out)
{
    static bool registered = false;
    static const char cls[] = "KiTTYBroadcastProbe";
    HINSTANCE hinst = GetModuleHandleA(NULL);
    HWND h;
    *out = NULL;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = kbc_probe_wndproc;
        wc.hInstance = hinst;
        wc.lpszClassName = cls;
        registered = RegisterClassA(&wc) != 0;
        if (!registered) return 0;
    }
    h = CreateWindowExA(0, cls, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hinst, NULL);
    if (!h) return 0;
    kbc_probe.e = NULL; kbc_probe.n = 0; kbc_probe.cap = 0;
    EnumWindows(kbc_probe_enum, (LPARAM)h);
    DestroyWindow(h);
    *out = kbc_probe.e;
    kbc_probe.e = NULL;
    return kbc_probe.n;
}

static int kbc_entry_cmp(const void *a, const void *b)
{
    const struct kbc_entry *x = (const struct kbc_entry *)a, *y = (const struct kbc_entry *)b;
    int c = strcmp(x->key, y->key);
    return c ? c : strcmp(x->name, y->name);
}

static void kbc_keys_free(void)
{
    int i;
    for (i = 0; i < kbc.nkeys; i++) sfree(kbc.keys[i]);
    sfree(kbc.keys);
    kbc.keys = NULL; kbc.nkeys = 0;
}

/* Rebuild both lists and the status line from the current view. */
static void kbc_fill(dlgparam *dlg)
{
    struct kbc_entry *e = NULL;
    int n, i;
    char *line;

    if (!kbc.list || !kbc.groups) return;
    n = kbc.live ? kbc_scan_live(&e) : kbc_scan_saved(&e);
    if (n > 1) qsort(e, n, sizeof(*e), kbc_entry_cmp);

    kbc_keys_free();
    for (i = 0; i < n; i++) {
        if (i == 0 || strcmp(e[i].key, e[i - 1].key) != 0) {
            kbc.keys = sresize(kbc.keys, kbc.nkeys + 1, char *);
            kbc.keys[kbc.nkeys++] = dupstr(e[i].key);
        }
    }

    /* The sessions, one heading row per key (the key in the first column,
     * id -1 like the column header), the sessions of that key beneath it. */
    dlg_update_start(kbc.list, dlg);
    dlg_listbox_clear(kbc.list, dlg);
    dlg_listbox_addwithid(kbc.list, dlg, KT_KSET_BC_COL_HEAD, -1);
    for (i = 0; i < n; i++) {
        if (i == 0 || strcmp(e[i].key, e[i - 1].key) != 0)
            dlg_listbox_addwithid(kbc.list, dlg, e[i].key, -1);
        line = dupprintf("    %s\t%s", e[i].name, e[i].key);
        dlg_listbox_addwithid(kbc.list, dlg, line, i);
        sfree(line);
    }
    dlg_update_done(kbc.list, dlg);

    /* The keys to send to. */
    dlg_update_start(kbc.groups, dlg);
    dlg_listbox_clear(kbc.groups, dlg);
    for (i = 0; i < kbc.nkeys; i++)
        dlg_listbox_addwithid(kbc.groups, dlg, kbc.keys[i], i);
    dlg_update_done(kbc.groups, dlg);

    if (!kbc.timer) {
        line = dupprintf(kbc.live ? KT_KSET_BC_STATUS_LIVE : KT_KSET_BC_STATUS_SAVED,
                         n, kbc.nkeys);
        dlg_label_change(kbc.status, dlg, line);
        sfree(line);
    }
    /* Only the live view can send: the saved view says so, and the button
     * follows it (and stays off while a send runs). */
    dlg_label_change(kbc.note, dlg, kbc.live ? " " : KT_KSET_BC_SWITCH_NOTE);
    kitty_dlg_enable_button(kbc.send, dlg, kbc.live && !kbc.timer);

    kbc_entries_free(e, n);
}

static void kbc_show_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    if (event == EVENT_REFRESH) {
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        dlg_listbox_addwithid(ctrl, dlg, KT_KSET_BC_SHOW_SAVED, 0);
        dlg_listbox_addwithid(ctrl, dlg, KT_KSET_BC_SHOW_LIVE, 1);
        dlg_listbox_select(ctrl, dlg, kbc.live ? 1 : 0);
        dlg_update_done(ctrl, dlg);
        kbc_fill(dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        kbc.live = (i == 1);
        kbc_fill(dlg);
    }
}

/* The lists and the command box keep their own state; nothing to do on the
 * framework's events. The command box is read when Send is pressed. */
static void kbc_noop_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
}

/* ---- the commands ------------------------------------------------------- */

/* Fill the box from a file. Empty lines are KEPT (each is a bare Return when
 * sent), a UTF-8 BOM is skipped, CRLF and LF both end a line, and the file's
 * trailing newline ends the last line rather than adding an empty one. */
static void kbc_load_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    char path[4096];
    FILE *fp;
    strbuf *sb;
    char chunk[4096];
    size_t n;
    if (event != EVENT_ACTION) return;
    path[0] = '\0';
    if (!OpenFileName(GetActiveWindow(), path, KT_KSET_BC_LOAD,
                      "Text files (*.txt)|*.txt|All files (*.*)|*.*|"))
        return;                                  /* cancelled */
    fp = fopen(path, "rb");
    if (!fp) {
        dlg_error_msg(dlg, KT_CFG_LOGINSCRIPT_OPEN_FAILED);
        return;
    }
    sb = strbuf_new();
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
        put_data(sb, chunk, n);
    fclose(fp);
    {
        strbuf *out = strbuf_new();
        const char *s = sb->s, *end = sb->s + sb->len;
        bool first = true;
        if (end - s >= 3 && (unsigned char)s[0] == 0xEF &&
            (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        while (s < end) {
            const char *nl = memchr(s, '\n', end - s);
            size_t len = nl ? (size_t)(nl - s) : (size_t)(end - s);
            if (len > 0 && s[len - 1] == '\r') len--;
            if (!first) put_dataz(out, "\r\n");
            first = false;
            put_data(out, s, len);
            s = nl ? nl + 1 : end;
        }
        dlg_editbox_set(kbc.cmds, dlg, out->s);
        strbuf_free(out);
    }
    strbuf_free(sb);
}

static void kbc_lines_free(void)
{
    int i;
    for (i = 0; i < kbc.nlines; i++) sfree(kbc.lines[i]);
    sfree(kbc.lines);
    kbc.lines = NULL; kbc.nlines = 0; kbc.at = 0;
    for (i = 0; i < kbc.nsendkeys; i++) sfree(kbc.sendkeys[i]);
    sfree(kbc.sendkeys);
    kbc.sendkeys = NULL; kbc.nsendkeys = 0;
}

/* One line to every selected group. An empty line is the "\n" escape: an
 * empty broadcast is dropped by sender and receiver alike, and the escape is
 * what SendKeyboardPlus turns into a bare Return. The send key is set per
 * group and put back afterwards, so a -sendcmdkey the process started with
 * survives the console. */
static void kbc_send_line(int at)
{
    int g;
    const char *line = kbc.lines[at];
    for (g = 0; g < kbc.nsendkeys; g++) {
        kitty_broadcast_set_send_key(kbc.sendkeys[g]);
        SendCommandAllWindowsEx(NULL, (char *)(*line ? line : "\\n"), 1);
    }
    kitty_broadcast_set_send_key(kbc.saved_override[0] ? kbc.saved_override : NULL);
}

static void kbc_progress(dlgparam *dlg)
{
    char *line = dupprintf(KT_KSET_BC_SENDING, kbc.at + 1, kbc.nlines);
    dlg_label_change(kbc.status, dlg, line);
    sfree(line);
}

/* The box is modeless: a loop with Sleep(commanddelay) between the lines would
 * freeze it, so the lines go out one per timer tick instead. */
static void CALLBACK kbc_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    if (!kbc.timer) return;
    if (!kbc.dlghwnd || !IsWindow(kbc.dlghwnd)) {
        /* The box went away under the send: stop, touch no control. */
        kbc_send_stop(false);
        return;
    }
    if (kbc.at + 1 >= kbc.nlines) {
        kbc_send_stop(true);
        return;
    }
    kbc.at++;
    kbc_progress(kbc.dlg);
    kbc_send_line(kbc.at);
}

static void kbc_send_stop(bool restore_status)
{
    if (kbc.timer) {
        KillTimer(NULL, kbc.timer);
        kbc.timer = 0;
    }
    kbc_lines_free();
    if (restore_status && kbc.dlg && kbc.dlghwnd && IsWindow(kbc.dlghwnd))
        kbc_fill(kbc.dlg);              /* the count line, and Send is back */
}

static void kbc_send_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    char *text;
    int i;
    if (event != EVENT_ACTION) return;
    if (!kbc.live || kbc.timer) return;   /* the saved view cannot send; one send at a time */

    /* The groups chosen. Nothing chosen = nothing to do. */
    for (i = 0; i < kbc.nkeys; i++) {
        if (dlg_listbox_issel(kbc.groups, dlg, i)) {
            kbc.sendkeys = sresize(kbc.sendkeys, kbc.nsendkeys + 1, char *);
            kbc.sendkeys[kbc.nsendkeys++] = dupstr(kbc.keys[i]);
        }
    }
    if (!kbc.nsendkeys) return;

    /* The lines: one command per line, CRLF or LF, the box's trailing newline
     * adds nothing. Empty lines stay - each is a bare Return. */
    text = dlg_editbox_get(kbc.cmds, dlg);
    {
        const char *s = text, *end = text + strlen(text);
        while (s < end) {
            const char *nl = memchr(s, '\n', end - s);
            size_t len = nl ? (size_t)(nl - s) : (size_t)(end - s);
            char *line;
            if (len > 0 && s[len - 1] == '\r') len--;
            line = snewn(len + 1, char);
            memcpy(line, s, len);
            line[len] = '\0';
            kbc.lines = sresize(kbc.lines, kbc.nlines + 1, char *);
            kbc.lines[kbc.nlines++] = line;
            s = nl ? nl + 1 : end;
        }
    }
    sfree(text);
    if (!kbc.nlines) { kbc_lines_free(); return; }

    snprintf(kbc.saved_override, sizeof(kbc.saved_override), "%s",
             kitty_broadcast_send_key_override());
    kbc.dlg = dlg;
    kbc.dlghwnd = dlg->hwnd;
    kbc.at = 0;
    kbc.timer = SetTimer(NULL, 0, autocommand_delay > 0 ? autocommand_delay : 5,
                         kbc_timer_proc);
    kitty_dlg_enable_button(kbc.send, dlg, false);
    kbc_progress(dlg);
    kbc_send_line(0);                     /* the first line goes now */
    if (kbc.nlines == 1)
        kbc_send_stop(true);
}

/* The box is closing: a send in flight stops, and whoever else hooked the
 * close (the Host keys panel) still gets its call. */
static void (*kbc_prev_closing_hook)(void) = NULL;
static void kbc_box_closing(void)
{
    kbc_send_stop(false);
    kbc.dlg = NULL; kbc.dlghwnd = NULL;
    kbc.keybox = kbc.keyprov = kbc.show = kbc.list = kbc.groups = NULL;
    kbc.cmds = kbc.load = kbc.send = kbc.status = kbc.note = NULL;
    kbc_keys_free();
    if (kbc_prev_closing_hook)
        kbc_prev_closing_hook();
}

static void kbc_leaf(struct controlbox *b)
{
    struct controlset *s;
    dlgcontrol *c;

    kbc_send_stop(false);
    kbc_keys_free();
    memset(&kbc, 0, sizeof(kbc));
    if (kitty_cfg_box_closing_hook != kbc_box_closing) {
        kbc_prev_closing_hook = kitty_cfg_box_closing_hook;
        kitty_cfg_box_closing_hook = kbc_box_closing;
    }

    ctrl_settitle(b, KSET_PATH("Automation/Broadcast"), KT_KSET_BC_TITLE);

    /* The master switch, moved off the Automation page. */
    s = ctrl_getset(b, KSET_PATH("Automation/Broadcast"), "broadcast", KT_KSET_AU_BROADCAST);
    KSET_CHECKBOX(s, KT_KSET_AU_SENDCMD, KI_SENDCMDMODE, kitty_kset_broadcast);
    ctrl_text(s, KT_KSET_AU_SENDCMD_NOTE, HELPCTX(kitty_kset_broadcast));

    /* The installation's group key: the same shape as Session > Broadcast
     * (label on its own line, box + Copy + Clear, one provenance line), so
     * the two panels read alike. The key is read when a window starts, so
     * a change here reaches the terminals opened afterwards. */
    s = ctrl_getset(b, KSET_PATH("Automation/Broadcast"), "groupkey", KT_KSET_BC_GROUPKEY);
    ctrl_text(s, KT_KSET_BC_GROUPKEY_LABEL, HELPCTX(kitty_kset_broadcast));
    ctrl_columns(s, 3, 60, 20, 20);
    c = ctrl_editbox(s, NULL, NO_SHORTCUT, 100, HELPCTX(kitty_kset_broadcast),
                     kbc_key_box_handler, P(NULL), ED_STR);
    c->column = 0;
    kbc.keybox = c;
    c = ctrl_pushbutton(s, KT_LOGGING_COPY, NO_SHORTCUT, HELPCTX(kitty_kset_broadcast),
                        kbc_key_copy_handler, P(NULL));
    c->column = 1;
    c = ctrl_pushbutton(s, KT_LOGGING_CLEAR, NO_SHORTCUT, HELPCTX(kitty_kset_broadcast),
                        kbc_key_clear_handler, P(NULL));
    c->column = 2;
    ctrl_columns(s, 1, 100);
    /* Built with the longer wording so the line's height fits both. */
    kbc.keyprov = ctrl_text(s, KT_KSET_BC_KEY_CUSTOM, HELPCTX(kitty_kset_broadcast));

    /* The send console. */
    s = ctrl_getset(b, KSET_PATH("Automation/Broadcast"), "send", KT_KSET_BC_SEND);
    kbc.show = ctrl_droplist(s, KT_KSET_BC_SHOW, NO_SHORTCUT, 60,
                             HELPCTX(kitty_kset_broadcast), kbc_show_handler, P(NULL));
    kbc.list = ctrl_listbox(s, NULL, NO_SHORTCUT, HELPCTX(kitty_kset_broadcast),
                            kbc_noop_handler, P(NULL));
    kbc.list->listbox.height = 5;
    kbc.list->listbox.headerrow = true;
    kbc.list->listbox.ncols = 2;
    kbc.list->listbox.percentages = snewn(2, int);
    kbc.list->listbox.percentages[0] = 45;   /* Session */
    kbc.list->listbox.percentages[1] = 55;   /* Key */
    kbc.status = ctrl_text(s, " ", HELPCTX(kitty_kset_broadcast));
    kbc.groups = ctrl_listbox(s, KT_KSET_BC_GROUPS, NO_SHORTCUT,
                              HELPCTX(kitty_kset_broadcast), kbc_noop_handler, P(NULL));
    kbc.groups->listbox.height = 3;
    kbc.groups->listbox.multisel = 1;        /* several groups at once */
    kbc.cmds = ctrl_editbox_multiline(s, KT_KSET_BC_COMMANDS, NO_SHORTCUT, 5, false,
                                      HELPCTX(kitty_kset_broadcast),
                                      kbc_noop_handler, P(NULL), P(NULL));
    ctrl_columns(s, 2, 50, 50);
    kbc.load = ctrl_pushbutton(s, KT_KSET_BC_LOAD, NO_SHORTCUT, HELPCTX(kitty_kset_broadcast),
                               kbc_load_handler, P(NULL));
    kbc.load->column = 0;
    kbc.send = ctrl_pushbutton(s, KT_KSET_BC_SEND_BTN, NO_SHORTCUT, HELPCTX(kitty_kset_broadcast),
                               kbc_send_handler, P(NULL));
    kbc.send->column = 1;
    ctrl_columns(s, 1, 100);
    /* Built with the note so its height fits; blank while the live view shows. */
    kbc.note = ctrl_text(s, KT_KSET_BC_SWITCH_NOTE, HELPCTX(kitty_kset_broadcast));
}

/* ==== The KiTTY++ Settings leaves, built from the kset table ============ */

static void scb_panel_kitty_settings_leaves(struct controlbox *b)
{
    struct controlset *s;
    char line[1400];
    char buf[4096];

    /* ---- Appearance: the colour theme of every window of the suite ---- */
    ctrl_settitle(b, KSET_PATH("Appearance"), KT_APPEARANCE_TITLE);
    s = ctrl_getset(b, KSET_PATH("Appearance"), "colours", KT_APPEARANCE_COLOURS);
    ctrl_droplist(s, KT_CONFIG_WINDOW_COLOURS, NO_SHORTCUT, 40, HELPCTX(kitty_appearance),
                  kitty_cfgwin_theme_handler, P(NULL));
    ctrl_text(s, KT_CONFIG_WINDOW_ONE_SETTING_FOR_THE_WHOLE, HELPCTX(kitty_appearance));
    /* Said beside the control: someone changes the colours and looks at
     * this window to see whether anything happened. It cannot - a theme is
     * applied to a window when it is created, and this one already was. */
    ctrl_text(s, KT_CONFIG_WINDOW_CHANGES_APPLY_TO_WINDOWS_OPENED, HELPCTX(kitty_appearance));

    /* ---- Keys & Mouse, and its Shortcuts leaf ---- */
    ctrl_settitle(b, KSET_PATH("Keys & Mouse"), KT_KSET_TW_TITLE);
    s = ctrl_getset(b, KSET_PATH("Keys & Mouse"), "behaviour", KT_KSET_TW_BEHAVIOUR);
    KSET_CHECKBOX(s, KT_KSET_TW_MOUSECHORDS, KI_MOUSESHORTCUTS, kitty_kset_terminal);
    ctrl_text(s, KT_KSET_TW_MOUSECHORDS_NOTE, HELPCTX(kitty_kset_terminal));
    /* Narrower droplist than the macro's: the label needs the room. */
    ctrl_droplist(s, KT_KSET_TW_FUNKEYS, NO_SHORTCUT, 40, HELPCTX(kitty_kset_terminal),
                  kitty_kset_handler, KSET(KI_FUNKEYS));
    ctrl_text(s, KT_KSET_TW_FUNKEYS_NOTE, HELPCTX(kitty_kset_terminal));

    ctrl_settitle(b, KSET_PATH("Keys & Mouse/Shortcuts"), KT_KSET_SC_TITLE);
    s = ctrl_getset(b, KSET_PATH("Keys & Mouse/Shortcuts"), "switch", NULL);
    KSET_CHECKBOX(s, KT_KSET_SC_ENABLE, KI_SHORTCUTS, kitty_kset_shortcuts);
    /* The switch decides whether keys fire; the editor below works either
     * way, its edits go to the file. */
    scb_panel_shortcut_editor(b, KSET_PATH("Keys & Mouse/Shortcuts"));

    /* ---- Automation ---- */
    ctrl_settitle(b, KSET_PATH("Automation"), KT_KSET_AU_TITLE);
    s = ctrl_getset(b, KSET_PATH("Automation"), "pacing", KT_KSET_AU_PACING);
    KSET_NUMBER(s, KT_KSET_AU_INITDELAY, KI_INITDELAY, kitty_kset_automation);
    KSET_NUMBER(s, KT_KSET_AU_BCDELAY, KI_BCDELAY, kitty_kset_automation);
    KSET_NUMBER(s, KT_KSET_AU_INTERNALDELAY, KI_INTERNALDELAY, kitty_kset_automation);
    KSET_NUMBER(s, KT_KSET_AU_COMMANDDELAY, KI_COMMANDDELAY, kitty_kset_automation);
    s = ctrl_getset(b, KSET_PATH("Automation"), "scripts", KT_KSET_AU_SCRIPTS);
    KSET_CHECKBOX(s, KT_KSET_AU_SCRIPTMODE, KI_SCRIPTMODE, kitty_kset_automation);
    KSET_TEXTBOX(s, KT_KSET_AU_SCRIPTFILTER, KI_SCRIPTFILEFILTER, kitty_kset_automation);
    ctrl_text(s, KT_KSET_AU_SCRIPTFILTER_NOTE, HELPCTX(kitty_kset_automation));
    /* The broadcast group (master switch, group key) and the send console
     * are the Automation > Broadcast leaf now; the tracing switch went to
     * Application > Security (it traces more than the automation). */
    kbc_leaf(b);

    /* ---- Features & Printing; the title bar, icons and font fallback
     * groups are how the windows LOOK and go to Appearance (ctrl_getset
     * appends to that panel wherever it is called from) ---- */
    ctrl_settitle(b, KSET_PATH("Terminal & Printing"), KT_KSET_WD_TITLE);
    s = ctrl_getset(b, KSET_PATH("Appearance"), "titlebar", KT_KSET_WD_TITLEBAR);
    /* The group moved to Appearance and its help must follow: with the old
     * context F1 opened "The Terminal & Printing panel". */
    KSET_CHECKBOX(s, KT_KSET_WD_WINTITLE, KI_WINTITLE, kitty_appearance);
    KSET_CHECKBOX(s, KT_KSET_WD_SIZE, KI_SIZE, kitty_appearance);
    KSET_CHECKBOX(s, KT_KSET_WD_WINROLL, KI_WINROLL, kitty_appearance);
    /* The shared terminal-window position, directly after Title bar (groups
     * appear in the order they are first created). Read-only, plus Reset. */
    {
        char lines[2][256];
        int i;
        s = ctrl_getset(b, KSET_PATH("Appearance"), "sharedpos", KT_KSET_WD_SHAREDPOS);
        ksharedpos_text(lines);
        for (i = 0; i < 2; i++)
            ksharedpos_lines[i] = ctrl_text(s, lines[i], HELPCTX(kitty_appearance));
        ctrl_pushbutton(s, KT_KSET_WD_SHAREDPOS_RESET, NO_SHORTCUT,
                        HELPCTX(kitty_appearance), kitty_sharedpos_reset_handler, I(0));
    }
    s = ctrl_getset(b, KSET_PATH("Terminal & Printing"), "features", KT_KSET_WD_FEATURES);
    KSET_DROPLIST(s, KT_KSET_WD_RENDERER, KI_RENDERER, kitty_kset_window);
    KSET_DROPLIST(s, KT_KSET_WD_FRAMEPACE, KI_FRAMEPACE, kitty_kset_window);
    ctrl_text(s, KT_KSET_WD_RENDERER_NOTE, HELPCTX(kitty_kset_window));
    KSET_CHECKBOX(s, KT_KSET_WD_CTRLTAB, KI_CTRLTAB, kitty_kset_window);
    kset_transparency_ctrl =
        KSET_CHECKBOX(s, KT_KSET_WD_TRANSPARENCY, KI_TRANSPARENCY, kitty_kset_window);
    KSET_CHECKBOX(s, KT_KSET_WD_BGIMAGE, KI_BGIMAGE, kitty_kset_window);
    KSET_CHECKBOX(s, KT_KSET_TW_HYPERLINK, KI_HYPERLINK, kitty_kset_window);
    KSET_NUMBER(s, KT_KSET_WD_SLIDEDELAY, KI_SLIDEDELAY, kitty_kset_window);
    KSET_CHECKBOX(s, KT_KSET_WD_SHRINK, KI_SHRINKBITMAP, kitty_kset_window);
    /* Not a feature: the library the per-session icon numbers index into
     * (kitty_startup.c loads it at startup, kitty.dll or the exe when unset). */
    s = ctrl_getset(b, KSET_PATH("Appearance"), "icons", KT_KSET_WD_ICONS);
    KSET_FILESEL(s, KT_KSET_WD_ICONFILE, KT_KSET_WD_ICONFILE_SELECT, KI_ICONFILE, kitty_appearance);
    ctrl_text(s, KT_KSET_WD_ICONFILE_NOTE, HELPCTX(kitty_appearance));
    s = ctrl_getset(b, KSET_PATH("Terminal & Printing"), "printing", KT_KSET_WD_PRINTING);
    KSET_NUMBER(s, KT_KSET_WD_PRINT_PITCH, KI_PRINT_HEIGHT, kitty_kset_window);
    KSET_NUMBER(s, KT_KSET_WD_PRINT_LINES, KI_PRINT_MAXLINE, kitty_kset_window);
    KSET_NUMBER(s, KT_KSET_WD_PRINT_CHARS, KI_PRINT_MAXCHAR, kitty_kset_window);
    ctrl_text(s, KT_KSET_WD_FILEONLY, HELPCTX(kitty_kset_window));
    s = ctrl_getset(b, KSET_PATH("Appearance"), "fontfb", KT_KSET_WD_FONTFB);
    KSET_CHECKBOX(s, KT_KSET_WD_FONTFB_ACTIVE, KI_FONTFALLBACK_ACTIVE, kitty_appearance);
    KSET_TEXTBOX(s, KT_KSET_WD_FONTFB_LIST, KI_FONTFALLBACK_FALLBACK, kitty_appearance);
    ctrl_text(s, KT_KSET_WD_FONTFB_LIST_NOTE, HELPCTX(kitty_appearance));
    ctrl_text(s, KT_KSET_WD_FONTFB_FILEONLY, HELPCTX(kitty_appearance));

    /* ---- Connection & reconnect ---- */
    ctrl_settitle(b, KSET_PATH("Reconnect & Prompts"), KT_KSET_CN_TITLE);
    s = ctrl_getset(b, KSET_PATH("Reconnect & Prompts"), "reconnect", KT_KSET_CN_RECONNECT);
    KSET_CHECKBOX(s, KT_KSET_CN_AUTORECONNECT, KI_AUTORECONNECT, kitty_kset_connection);
    ctrl_text(s, KT_KSET_CN_AUTORECONNECT_NOTE, HELPCTX(kitty_kset_connection));
    KSET_NUMBER(s, KT_KSET_CN_DELAY, KI_RECONNECTDELAY, kitty_kset_connection);
    s = ctrl_getset(b, KSET_PATH("Reconnect & Prompts"), "confirm", KT_KSET_CN_CONFIRM);
    KSET_CHECKBOX(s, KT_KSET_CN_MODALERRORS, KI_MODALERRORS, kitty_kset_connection);
    KSET_DROPLIST(s, KT_KSET_CN_NEWKEY, KI_MODALNEWHOSTKEYCONFIRMATION, kitty_kset_connection);
    KSET_DROPLIST(s, KT_KSET_CN_CHANGEDKEY, KI_MODALCHANGEDHOSTKEYCONFIRMATION, kitty_kset_connection);
    KSET_DROPLIST(s, KT_KSET_CN_WEAKKEY, KI_MODALWEAKKEYCONFIRMATION, kitty_kset_connection);

    /* ---- Named Proxies > Proxy-Forwards: the jump-host chain limit is a
     * named-proxy matter, so it lives under that panel ---- */
    ctrl_settitle(b, "Application/Named Proxies/Proxy-Forwards", KT_PXFWD_TITLE);
    s = ctrl_getset(b, "Application/Named Proxies/Proxy-Forwards", "chains", KT_PXFWD_CHAINS);
    KSET_NUMBER(s, KT_KSET_CN_CHAINMAX, KI_PROXYCHAINMAX, kitty_proxy_forwards);
    ctrl_text(s, KT_PXFWD_NOTE, HELPCTX(kitty_proxy_forwards));

    /* ---- Transfers & Tools: the helper programs, with WinSCP and ZModem
     * as leaves of their own (they were "External tools") ---- */
    ctrl_settitle(b, KSET_PATH("Transfers & Tools"), KT_KSET_TT_TITLE);
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools"), "kscp", KT_KSET_TT_KSCP);
    KSET_FILESEL(s, KT_KSET_TT_PSCPPATH, KT_KSET_TT_PSCPPATH_SELECT, KI_PSCPPATH, kitty_helper_paths);
    ctrl_text(s, KT_KSET_TT_PSCPPATH_NOTE, HELPCTX(kitty_helper_paths));
    /* What the search found at this start, so a blank field still tells
     * the reader which binary is in use. */
    buf[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_PSCPPATH, buf, sizeof(buf));
    if (!buf[0]) {
        snprintf(line, sizeof(line), KT_KSET_TT_PSCPPATH_FOUND,
                 PSCPPath && PSCPPath[0] ? PSCPPath : KT_KSET_TT_PSCPPATH_NONE);
        ctrl_text(s, line, HELPCTX(kitty_helper_paths));
    }
    KSET_TEXTBOX(s, KT_KSET_TT_PSCPPORT, KI_PSCPPORT, kitty_helper_paths);
    {
        /* The download folder is local: a folder picker beside it. The
         * button's context is the box it fills. */
        kitty_controls_set_dir_picker(OpenDirNameFrom);   /* the row's Locate... */
        ctrl_filesel(s, KT_KSET_TT_DOWNLOADDIR, NO_SHORTCUT, FILTER_FOLDERS,
                     false, NULL, HELPCTX(kitty_helper_paths),
                     kitty_kset_handler, KSET(KI_DOWNLOADDIR));
        ctrl_text(s, KT_KSET_TT_DOWNLOADDIR_NOTE, HELPCTX(kitty_helper_paths));
        /* The upload folder is local as well: where Send File opens and
         * where a name the far end asks to read (kitten transfer) is looked
         * up. Never the remote target of an upload. */
        ctrl_filesel(s, KT_KSET_TT_UPLOADDIR, NO_SHORTCUT, FILTER_FOLDERS,
                     false, NULL, HELPCTX(kitty_helper_paths),
                     kitty_kset_handler, KSET(KI_UPLOADDIR));
        ctrl_text(s, KT_KSET_TT_UPLOADDIR_NOTE, HELPCTX(kitty_helper_paths));
    }
    KSET_CHECKBOX(s, KT_KSET_TT_NOTIFY, KI_TRANSFERNOTIFICATION, kitty_helper_paths);

    /* The global defaults of the two kitten transfer limits; the session's
     * OSC 5113 (kitten transfer) group on Connection > File-Transfer-Settings overrides
     * them per session. */
    ctrl_settitle(b, KSET_PATH("Transfers & Tools/OSC 5113 (kitten)"), KT_KSET_KITTEN_TITLE);
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools/OSC 5113 (kitten)"), "defaults", KT_KSET_KITTEN_DEFAULTS);
    KSET_NUMBER(s, KT_TRANSFERS_MAX_MB, KI_TRANSFERMAXMB, kitty_kset_kitten);
    ctrl_text(s, KT_KSET_KITTEN_MAX_NOTE, HELPCTX(kitty_kset_kitten));
    KSET_CHECKBOX(s, KT_TRANSFERS_FULL_PATH, KI_TRANSFERFULLPATH, kitty_kset_kitten);
    ctrl_text(s, KT_KSET_KITTEN_NOTE, HELPCTX(kitty_kset_kitten));

    ctrl_settitle(b, KSET_PATH("Transfers & Tools/WinSCP"), KT_WINSCP_WINSCP);
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools/WinSCP"), "path", KT_WINSCP_EXECUTABLE);
    ctrl_filesel(s, KT_WINSCP_WINSCP_EXECUTABLE, NO_SHORTCUT,
                 FILTER_ALL_FILES, false, KT_WINSCP_SELECT_WINSCP_EXECUTABLE,
                 HELPCTX(kitty_helper_paths), kitty_toolpath_handler, P(KI_WINSCPPATH));
    /* kitty_helper_paths = "The Transfers & Tools panel", which describes the
     * helper programs; kitty_winscp is the KSCP panel's topic. */
    ctrl_text(s, KT_WINSCP_THE_OTHER_WINSCP_SETTINGS_BELONG, HELPCTX(kitty_helper_paths));

    ctrl_settitle(b, KSET_PATH("Transfers & Tools/FileZilla"), KT_FZ_FILEZILLA);
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools/FileZilla"), "path", KT_WINSCP_EXECUTABLE);
    ctrl_filesel(s, KT_FZ_EXECUTABLE, NO_SHORTCUT,
                 FILTER_ALL_FILES, false, KT_FZ_SELECT_EXECUTABLE,
                 HELPCTX(kitty_helper_paths), kitty_toolpath_handler, P(KI_FILEZILLAPATH));
    ctrl_text(s, KT_FZ_THE_OTHER_SETTINGS_BELONG, HELPCTX(kitty_helper_paths));

    ctrl_settitle(b, KSET_PATH("Transfers & Tools/ZModem"), KT_ZMODEM_ZMODEM);
    /* The installation-wide switch ([KiTTY] zmodem) sits on this leaf, which
     * is always built. A session's ZModem panel is not built with the switch
     * off and points here instead. */
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools/ZModem"), "global", NULL);
    ctrl_checkbox(s, KT_ZMODEM_GLOBAL_ENABLE, NO_SHORTCUT,
                  HELPCTX(kitty_zmodem), kitty_kset_handler,
                  P((void *)kset_find(KI_ZMODEM)));
    s = ctrl_getset(b, KSET_PATH("Transfers & Tools/ZModem"), "cmds", KT_EXTERNAL_TOOLS_HELPER_PROGRAMS);
    ctrl_filesel(s, KT_ZMODEM_RECEIVE_COMMAND_RZ_2, NO_SHORTCUT,
                 FILTER_ALL_FILES, false,
                 KT_ZMODEM_SELECT_COMMAND_TO_RECEIVE_ZMODEM,
                 HELPCTX(kitty_zmodem), kitty_toolpath_handler, P(KI_RZCOMMAND));
    ctrl_filesel(s, KT_ZMODEM_SEND_COMMAND_SZ_2, NO_SHORTCUT,
                 FILTER_ALL_FILES, false,
                 KT_ZMODEM_SELECT_COMMAND_TO_SEND_ZMODEM,
                 HELPCTX(kitty_zmodem), kitty_toolpath_handler, P(KI_SZCOMMAND));
    /* Two lines, two controls: the panel machinery takes no newline. */
    ctrl_text(s, KT_ZMODEM_NOTE_OPTIONS, HELPCTX(kitty_zmodem));
    ctrl_text(s, KT_ZMODEM_NOTE_RECEIVED, HELPCTX(kitty_zmodem));

    /* ---- Launcher ---- */
    ctrl_settitle(b, KSET_PATH("Launcher"), KT_KSET_LA_TITLE);
    s = ctrl_getset(b, KSET_PATH("Launcher"), "intro", NULL);
    ctrl_text(s, KT_KSET_LA_READ_AT_START, HELPCTX(kitty_kset_launcher));
    s = ctrl_getset(b, KSET_PATH("Launcher"), "menu", KT_KSET_LA_MENU);
    KSET_CHECKBOX(s, KT_KSET_LA_RELOAD, KI_LAUNCHER_RELOAD, kitty_kset_launcher);
    KSET_DROPLIST(s, KT_KSET_LA_SECOND, KI_LAUNCHER_ALREADYRUNCHECK, kitty_kset_launcher);
    s = ctrl_getset(b, KSET_PATH("Launcher"), "workplace", KT_KSET_LA_WORKPLACE);
    KSET_CHECKBOX(s, KT_KSET_LA_EXITWITH, KI_LAUNCHER_EXITWITHWORKPLACE, kitty_kset_launcher);
    KSET_NUMBER(s, KT_KSET_LA_NOTICE, KI_LAUNCHER_NOTICESECONDS, kitty_kset_launcher);
    /* [Launcher] classname is deliberately NOT shown: it exists only to keep
     * two installations' launchers from taking each other for "already
     * running", is set by hand in kitty.ini for that one purpose, and a
     * line reporting it told nobody anything. The help explains it. */

    /* ---- System: the Windows shell integration ---- */
    {
        char lines[5][256];
        dlgcontrol *bc;
        int i;
        kitty_shell_integration_state(lines);
        ctrl_settitle(b, KSET_PATH("System"), KT_SYSTEM_TITLE);
        s = ctrl_getset(b, KSET_PATH("System"), "state", KT_SYSTEM_STATE_GROUP);
        for (i = 0; i < 5; i++)
            ksys_lines[i] = ctrl_text(s, lines[i], HELPCTX(kitty_system));
        s = ctrl_getset(b, KSET_PATH("System"), "register", KT_SYSTEM_REGISTER_GROUP);
        ctrl_text(s, KT_SYSTEM_NOTE, HELPCTX(kitty_system));
        /* One full-width button per action, each under the line that says
         * what it does: two side by side were too narrow for their words,
         * and told nobody how they differed. */
        ctrl_text(s, KT_SYSTEM_REGISTER_LINE, HELPCTX(kitty_system));
        bc = ctrl_pushbutton(s, KT_SYSTEM_REGISTER, NO_SHORTCUT,
                             HELPCTX(kitty_system), kitty_system_handler, I(0));
        ctrl_text(s, KT_SYSTEM_TAKEOVER_LINE, HELPCTX(kitty_system));
        bc = ctrl_pushbutton(s, KT_SYSTEM_TAKEOVER, NO_SHORTCUT,
                             HELPCTX(kitty_system), kitty_system_handler, I(1));
        ctrl_text(s, KT_SYSTEM_UNREGISTER_LINE, HELPCTX(kitty_system));
        bc = ctrl_pushbutton(s, KT_SYSTEM_UNREGISTER, NO_SHORTCUT,
                             HELPCTX(kitty_system), kitty_system_handler, I(2));
        (void)bc;
        /* The command-line tools reachable from any shell: this folder on
         * the user's PATH. The box shows the registry's state, not a
         * setting of ours (kitty_userpath.c). */
        s = ctrl_getset(b, KSET_PATH("System"), "path", KT_SYSTEM_PATH_GROUP);
        ctrl_checkbox(s, KT_SYSTEM_PATH_CHECK, NO_SHORTCUT, HELPCTX(kitty_system),
                      kitty_syspath_handler, P(NULL));
        ctrl_text(s, KT_SYSTEM_PATH_NOTE, HELPCTX(kitty_system));
    }
}

/* ---- Storage & Backup > KiTTY.ini ------------------------------------- */

/* The file the view is showing, "" when there is none. */
static const char *iniview_path(const struct iniview_data *iv)
{
    return iv->showing ? iv->example_path : iv->ini_path;
}

/* Is this line's key one whose value must not be shown? A user may put a
 * secret into kitty.ini by hand, and a screenshot of this panel must be safe
 * to share. Comment lines (the example is all comments) are shown as they
 * are; only a live "key=value" is masked - and only when the key ENDS with
 * the word: "passphrasecacheseconds" is a number and
 * "PortablePasswordProtection" a mode, and a substring test hid both. */
static bool iniview_key_is_secret(const char *line, size_t keylen)
{
    static const char *const words[] = { "password", "passphrase", "secret", "token" };
    char key[128];
    size_t i, w;
    while (keylen > 0 && (line[keylen - 1] == ' ' || line[keylen - 1] == '\t'))
        keylen--;                      /* "key = value" spacing */
    if (keylen == 0 || keylen >= sizeof(key))
        return false;
    for (i = 0; i < keylen; i++)
        key[i] = (char)tolower((unsigned char)line[i]);
    key[keylen] = '\0';
    for (w = 0; w < lenof(words); w++) {
        size_t wl = strlen(words[w]);
        if (keylen >= wl && !strcmp(key + keylen - wl, words[w]))
            return true;
    }
    return false;
}

/* The file's text for the edit box: CRLF line ends (a LF-only file shows as
 * one line otherwise), secrets masked. Caller frees. */
static char *iniview_read(const char *path, FILETIME *written, bool *ok)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    FILE *fp;
    long len;
    char *raw, *p, *end;
    strbuf *out;

    *ok = false;
    memset(written, 0, sizeof(*written));
    if (!path || !path[0])
        return dupstr(KT_INIVIEW_NO_FILE);
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        *written = fad.ftLastWriteTime;
    fp = fopen(path, "rb");
    if (!fp)
        return dupstr(KT_INIVIEW_NOT_FOUND);
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) len = 0;
    raw = snewn((size_t)len + 1, char);
    len = (long)fread(raw, 1, (size_t)len, fp);
    fclose(fp);
    raw[len] = '\0';
    *ok = true;

    out = strbuf_new();
    p = raw;
    while (*p) {
        end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        size_t body = n;
        const char *eq;
        if (body && p[body - 1] == '\r')
            body--;
        eq = memchr(p, '=', body);
        if (eq && p[0] != ';' && p[0] != '#' &&
            iniview_key_is_secret(p, (size_t)(eq - p))) {
            put_data(out, p, (size_t)(eq - p) + 1);
            put_dataz(out, KT_INIVIEW_MASK);
        } else {
            put_data(out, p, body);
        }
        put_datapl(out, PTRLEN_LITERAL("\r\n"));
        if (!end)
            break;
        p = end + 1;
    }
    smemclr(raw, (size_t)len);
    sfree(raw);
    return strbuf_to_str(out);
}

static void iniview_load(struct iniview_data *iv, dlgparam *dlg, bool keep_scroll)
{
    HWND h = kitty_cfg_ctrl_hwnd(iv->view);
    int first = 0;
    char *text;

    if (h && keep_scroll)
        first = (int)SendMessage(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    text = iniview_read(iniview_path(iv), &iv->shown_write, &iv->shown_valid);
    dlg_editbox_set(iv->view, dlg, text);
    smemclr(text, strlen(text));
    sfree(text);
    if (h && keep_scroll && first > 0)
        SendMessage(h, EM_LINESCROLL, 0, first);
}

/* Once a second while the leaf is on screen: a changed last-write time
 * replaces the text, keeping the scroll position. A stat per second, no
 * change notification to own and close. */
void kitty_iniview_poll(dlgparam *dlg)
{
    struct iniview_data *iv = kitty_iniview_active;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    const char *path;

    if (!iv || !iv->view || !dlg || !dlg_is_visible(iv->view, dlg))
        return;
    path = iniview_path(iv);
    if (!path[0])
        return;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        if (iv->shown_valid)              /* the file went away */
            iniview_load(iv, dlg, false);
        return;
    }
    if (!iv->shown_valid ||
        CompareFileTime(&fad.ftLastWriteTime, &iv->shown_write) != 0)
        iniview_load(iv, dlg, true);
}

static void kitty_iniview_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                  void *data, int event)
{
    struct iniview_data *iv = (struct iniview_data *)ctrl->context.p;

    if (ctrl == iv->show) {
        if (event == EVENT_REFRESH) {
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            dlg_listbox_addwithid(ctrl, dlg, KT_INIVIEW_SHOW_INI, 0);
            if (iv->example_path[0])
                dlg_listbox_addwithid(ctrl, dlg, KT_INIVIEW_SHOW_EXAMPLE, 1);
            dlg_listbox_select(ctrl, dlg, iv->showing);
            dlg_update_done(ctrl, dlg);
        } else if (event == EVENT_SELCHANGE) {
            int i = dlg_listbox_index(ctrl, dlg);
            iv->showing = (i > 0 && iv->example_path[0]) ? 1 : 0;
            iniview_load(iv, dlg, false);
        }
    } else if (ctrl == iv->view) {
        if (event == EVENT_REFRESH)
            iniview_load(iv, dlg, false);
        /* EVENT_VALCHANGE: the box is read-only; dlg_editbox_set fires it,
         * and there is nothing to store. */
    } else if (ctrl == iv->editbtn && event == EVENT_ACTION) {
        if (!iv->ini_path[0]) {
            MessageBoxA(kitty_cfg_modal_owner(), KT_INIVIEW_NO_FILE, KT_CAP_KITTY,
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (MessageBoxA(kitty_cfg_modal_owner(), KT_INIVIEW_EDIT_WARN, KT_CAP_KITTY,
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return;
        /* The user's editor, never a write of our own. "edit" is the verb an
         * .ini file registers; "open" is the fallback for a machine that
         * has none. */
        if ((INT_PTR)ShellExecuteA(dlg->hwnd, "edit", iv->ini_path, NULL,
                                   NULL, SW_SHOWNORMAL) <= 32)
            ShellExecuteA(dlg->hwnd, "open", iv->ini_path, NULL, NULL,
                          SW_SHOWNORMAL);
    }
}

/* The leaf itself. `ini` is GetKittyIniFile() as the caller has it. */
static void scb_panel_iniview(struct controlbox *b, const char *ini)
{
    static const char *const path =
        KCFG_PATH_INIVIEW;
    struct iniview_data *iv;
    struct controlset *s;
    dlgcontrol *c;
    char line[MAX_PATH * 2 + 64];

    iv = (struct iniview_data *)ctrl_alloc(b, sizeof(*iv));
    memset(iv, 0, sizeof(*iv));
    kitty_iniview_active = iv;
    if (ini && ini[0] && !GetNoKittyFileFlag())
        snprintf(iv->ini_path, sizeof(iv->ini_path), "%s", ini);
    /* The example ships beside the executable (MSI and ZIP alike). */
    {
        char exe[MAX_PATH];
        char *bs;
        if (GetModuleFileNameA(NULL, exe, sizeof(exe)) &&
            (bs = strrchr(exe, '\\')) != NULL) {
            *bs = '\0';
            snprintf(iv->example_path, sizeof(iv->example_path),
                     "%s\\kitty.ini.example", exe);
            if (!existfile(iv->example_path))
                iv->example_path[0] = '\0';
        }
    }

    ctrl_settitle(b, path, KT_INIVIEW_TITLE);
    s = ctrl_getset(b, path, "show", NULL);
    iv->show = ctrl_droplist(s, KT_INIVIEW_SHOW, NO_SHORTCUT, 60,
                             HELPCTX(kitty_ini_view), kitty_iniview_handler,
                             P(iv));
    /* One path line, the configuration file's: a full path wraps to two or
     * three rows at this font, and the panel has to fit the window's minimum
     * with the view still showing something. The example's place is said by
     * its dropdown entry (it ships beside kitty.exe); its absence is said
     * here, because then the dropdown has only one entry and no reason. */
    snprintf(line, sizeof(line), KT_INIVIEW_PATH_INI,
             iv->ini_path[0] ? iv->ini_path : KT_INIVIEW_NONE);
    ctrl_text(s, line, HELPCTX(kitty_ini_view));
    if (!iv->example_path[0])
        ctrl_text(s, KT_INIVIEW_NO_EXAMPLE, HELPCTX(kitty_ini_view));

    /* Four rows is the FLOOR: the fill hook grows the box to the window. */
    s = ctrl_getset(b, path, "view", NULL);
    iv->view = ctrl_editbox_multiline(s, NULL, NO_SHORTCUT, 4, true,
                                      HELPCTX(kitty_ini_view),
                                      kitty_iniview_handler, P(iv), P(NULL));
    ctrl_text(s, KT_INIVIEW_TAKES_EFFECT, HELPCTX(kitty_ini_view));
    ctrl_columns(s, 2, 70, 30);
    c = ctrl_pushbutton(s, KT_INIVIEW_EDIT, NO_SHORTCUT,
                        HELPCTX(kitty_ini_view), kitty_iniview_handler, P(iv));
    c->column = 1;
    iv->editbtn = c;
    ctrl_columns(s, 1, 100);
}

/* ---- Keys & Mouse > Shortcuts: the shortcut editor -------------------------
 *
 * Two header-row lists over the [Shortcuts] section of kitty.ini, on two
 * leaves: every table action with the key it has (an unassigned one shows
 * an empty Key cell) on Shortcuts, and the AutoText entries - the key
 * combinations that type a text - on its AutoText sub-leaf.
 * A click on a row puts its key into the capture field under the list;
 * Save writes the row back in the {CONTROL}{F4} syntax, Default puts the
 * built-in key back, Delete and New serve the AutoText list. Every write
 * goes to kitty.ini and is followed by InitShortcuts(), so the key is live
 * in every window of this process, then the lists are rebuilt.
 *
 * The capture field is a read-only edit subclassed for WM_KEYDOWN: the key
 * pressed is taken with the modifiers held (the key state, plus the
 * modifier keys seen as messages, so a posted sequence captures too) and
 * shown as ShortcutKeyText(); Backspace or Delete clears it. Tab and
 * Escape keep their dialog meaning, Alt+F4 closes the box: none of the
 * three can be captured. */

enum { SC_CTX_ALIST, SC_CTX_AKEY, SC_CTX_ASAVE, SC_CTX_ADEFAULT,
       SC_CTX_TLIST, SC_CTX_TKEY, SC_CTX_TTEXT, SC_CTX_TSAVE, SC_CTX_TDELETE,
       SC_CTX_TNEW };
enum { SC_SUBCLASS_ACTIONS = 7, SC_SUBCLASS_TEXTS = 8 };

struct sc_text { char name[64]; char text[512]; int code; };
#define SC_TEXTS_MAX 64                 /* AutoText rows the editor lists */

struct sc_data {
    dlgparam *dlg;
    dlgcontrol *alist, *akey, *asave, *adefault, *anote;
    dlgcontrol *tlist, *tkey, *ttext, *tsave, *tdelete, *tnew, *tnote;
    int asel;                       /* the selected action, -1 = none */
    int acode;                      /* the key in its capture field */
    int asort_col; bool asort_desc;
    struct sc_text texts[SC_TEXTS_MAX]; int ntexts;
    int tsel;                       /* the selected AutoText row, -1 = none / New */
    int tcode;
    bool tediting;                  /* a row is selected, or New was pressed */
    int tsort_col; bool tsort_desc;
    /* modifier keys seen as key messages by a capture field */
    int mod_shift, mod_control, mod_alt, mod_altgr, mod_win;
};
static struct sc_data *kitty_sc_active;


dlgcontrol *kitty_sc_fill_ctrl(bool autotext)
{
    if (!kitty_sc_active) return NULL;
    return autotext ? kitty_sc_active->tlist : kitty_sc_active->alist;
}

static HWND sc_hwnd(dlgcontrol *ctrl)
{
    return ctrl ? kitty_cfg_ctrl_hwnd(ctrl) : NULL;
}

static void sc_enable(struct sc_data *sc, dlgcontrol *ctrl, bool on)
{
    if (ctrl && sc->dlg)
        kitty_wpmode_enable_ctrl(ctrl, sc->dlg, on);
}

static void sc_report(const char *text)
{
    kitty_info_box(kitty_cfg_modal_owner(), KT_KSET_TITLE, text, NULL);
}

/* Is there a kitty.ini a write can land in? Reports why not otherwise. */
static bool sc_can_write(void)
{
    const char *ini = GetKittyIniFile();
    if (GetNoKittyFileFlag() || !ini || !ini[0]) { sc_report(KT_KSET_SC_NO_INI); return false; }
    if (GetReadOnlyFlag()) { sc_report(KT_KSET_SC_READONLY); return false; }
    return true;
}

/* The key text of a code, or the ini spelling when the code has no name
 * (a numeric AutoText key): the row must show something. */
static void sc_key_text(int code, const char *fallback, char *buf, size_t size)
{
    if (!ShortcutKeyText(code, buf, size))
        snprintf(buf, size, "%s", fallback ? fallback : "");
}

/* The collision rule, both lists, both buttons: a code is refused when
 * another action holds it, an AutoText key holds it (other than the row
 * being edited), KiTTY keeps it, or it is Alt+F4. True = refused, and the
 * message has been shown. */
static bool sc_collides(struct sc_data *sc, int code, int skip_action, int skip_text)
{
    char key[64], msg[256];
    int i;
    if (!code) return false;
    sc_key_text(code, NULL, key, sizeof(key));
    if (ShortcutKeyReserved(code) == 2) { sc_report(KT_KSET_SC_ALTF4); return true; }
    if (ShortcutKeyReserved(code) == 1) {
        snprintf(msg, sizeof(msg), KT_KSET_SC_RESERVED, key); sc_report(msg); return true;
    }
    for (i = 0; i < ShortcutActionCount(); i++)
        if (i != skip_action && ShortcutActionValue(i) == code) {
            snprintf(msg, sizeof(msg), KT_KSET_SC_USED_BY_ACTION, key, ShortcutActionName(i));
            sc_report(msg); return true;
        }
    for (i = 0; i < sc->ntexts; i++)
        if (i != skip_text && sc->texts[i].code == code) {
            snprintf(msg, sizeof(msg), KT_KSET_SC_USED_BY_TEXT, key); sc_report(msg); return true;
        }
    return false;
}

/* ---- the actions list ---- */

static struct sc_data *sc_sort_sc;
static int sc_cmp_action(const void *av, const void *bv)
{
    int a = *(const int *)av, b = *(const int *)bv, c = 0;
    if (sc_sort_sc->asort_col == 1) {
        char ka[64], kb[64];
        sc_key_text(ShortcutActionValue(a), NULL, ka, sizeof(ka));
        sc_key_text(ShortcutActionValue(b), NULL, kb, sizeof(kb));
        c = stricmp(ka, kb);
    }
    if (!c) c = stricmp(ShortcutActionName(a), ShortcutActionName(b));
    return sc_sort_sc->asort_desc ? -c : c;
}

/* One fill for both lists: the entries' indexes sorted by cmp, the header
 * row, one two-column row per entry with its index as the row id, and the
 * selected entry selected again. row() writes the two cells of entry i. */
typedef void (*sc_row_fn)(struct sc_data *sc, int i, char *left, size_t lsize,
                          char *right, size_t rsize);
static void sc_fill_list(struct sc_data *sc, dlgcontrol *list, int n, int sel,
                         const char *head, int (*cmp)(const void *, const void *),
                         sc_row_fn row)
{
    int i, *order;
    if (!sc->dlg) return;
    order = snewn(n + 1, int);
    for (i = 0; i < n; i++) order[i] = i;
    sc_sort_sc = sc;
    qsort(order, n, sizeof(int), cmp);
    dlg_update_start(list, sc->dlg);
    dlg_listbox_clear(list, sc->dlg);
    dlg_listbox_addwithid(list, sc->dlg, head, -1);
    for (i = 0; i < n; i++) {
        char left[512], right[512], *text;
        row(sc, order[i], left, sizeof(left), right, sizeof(right));
        text = dupprintf("%s\t%s", left, right);
        dlg_listbox_addwithid(list, sc->dlg, text, order[i]);
        sfree(text);
        if (order[i] == sel)
            dlg_listbox_select(list, sc->dlg, i + 1);
    }
    dlg_update_done(list, sc->dlg);
    sfree(order);
}

/* an action row: its name, then the key it has (empty when unassigned) */
static void sc_action_row(struct sc_data *sc, int i, char *left, size_t lsize,
                          char *right, size_t rsize)
{
    snprintf(left, lsize, "%s", ShortcutActionName(i));
    sc_key_text(ShortcutActionValue(i), NULL, right, rsize);
}

static void sc_fill_actions(struct sc_data *sc)
{
    sc_fill_list(sc, sc->alist, ShortcutActionCount(), sc->asel,
                 KT_KSET_SC_ACTIONS_HEAD, sc_cmp_action, sc_action_row);
}

/* The line under a key field (the actions leaf and the AutoText leaf
 * have one each): which user-command slot a Ctrl+Shift+letter takes
 * away; blank for any other key. */
static void sc_note(struct sc_data *sc, dlgcontrol *note, int code)
{
    char key[64], line[256];
    int slot = ShortcutKeyUserCommand(code);
    if (!note || !sc->dlg) return;
    if (slot) {
        sc_key_text(code, NULL, key, sizeof(key));
        snprintf(line, sizeof(line), KT_KSET_SC_USERCMD_NOTE, key, slot);
        dlg_label_change(note, sc->dlg, line);
    } else {
        dlg_label_change(note, sc->dlg, " ");
    }
}

static void sc_show_action(struct sc_data *sc)
{
    char key[64];
    bool on = sc->asel >= 0;
    sc_key_text(sc->acode, NULL, key, sizeof(key));
    dlg_editbox_set(sc->akey, sc->dlg, on ? key : "");
    sc_enable(sc, sc->akey, on);
    sc_enable(sc, sc->asave, on);
    sc_enable(sc, sc->adefault, on);
    sc_note(sc, sc->anote, sc->acode);
}

/* Save (actions): the captured key, or the empty value, to kitty.ini. */
static void sc_save_action(struct sc_data *sc, int code)
{
    char syntax[64];
    if (sc->asel < 0) return;
    if (sc_collides(sc, code, sc->asel, -1)) return;
    if (!sc_can_write()) return;
    syntax[0] = '\0';
    if (code) ShortcutKeySyntax(code, syntax, sizeof(syntax));
    writeINI(GetKittyIniFile(), KI_SECTION_SHORTCUTS, ShortcutActionKey(sc->asel), syntax);
    InitShortcuts();
    sc->acode = ShortcutActionValue(sc->asel);
    sc_fill_actions(sc);
    sc_show_action(sc);
}

/* ---- the AutoText list ---- */

/* The entries of the [Shortcuts] list line, each with its own line: read
 * as InitShortcuts reads them, the text raw (the escapes as typed). */
static void sc_read_texts(struct sc_data *sc)
{
    char list[4096], *p, *q;
    sc->ntexts = 0;
    if (!ReadParameterN(KI_SECTION_SHORTCUTS, KI_SC_LIST, list, sizeof(list)))
        return;
    p = list;
    while (*p && sc->ntexts < SC_TEXTS_MAX) {
        struct sc_text *t = &sc->texts[sc->ntexts];
        while (*p == ' ') p++;
        if (!*p) break;
        q = p;
        while (*q && *q != ' ') q++;
        if (*q) *q++ = '\0';
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", p);
        if (ReadParameterN(KI_SECTION_SHORTCUTS, t->name, t->text, sizeof(t->text))) {
            t->code = (t->name[0] >= '0' && t->name[0] <= '9') ? atoi(t->name) : DefineShortcuts(t->name);
            sc->ntexts++;
        }
        p = q;
    }
}

/* The list line rewritten with one name taken out and one put in (either
 * may be NULL). The names of parked entries - in the list, no line of
 * their own - stay where they were. */
static void sc_write_list(const char *remove, const char *add)
{
    char list[4096], out[4096], *p, *q;
    bool present = false;
    out[0] = '\0';
    if (!ReadParameterN(KI_SECTION_SHORTCUTS, KI_SC_LIST, list, sizeof(list)))
        list[0] = '\0';
    p = list;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        q = p;
        while (*q && *q != ' ') q++;
        if (*q) *q++ = '\0';
        if (remove && !strcmp(p, remove)) { p = q; continue; }
        if (add && !strcmp(p, add)) present = true;
        if (strlen(out) + strlen(p) + 2 < sizeof(out)) {
            if (out[0]) strcat(out, " ");
            strcat(out, p);
        }
        p = q;
    }
    if (add && !present && strlen(out) + strlen(add) + 2 < sizeof(out)) {
        if (out[0]) strcat(out, " ");
        strcat(out, add);
    }
    writeINI(GetKittyIniFile(), KI_SECTION_SHORTCUTS, KI_SC_LIST, out);
}

static int sc_cmp_text(const void *av, const void *bv)
{
    const struct sc_text *a = &sc_sort_sc->texts[*(const int *)av];
    const struct sc_text *b = &sc_sort_sc->texts[*(const int *)bv];
    int c = 0;
    if (sc_sort_sc->tsort_col == 1) c = stricmp(a->text, b->text);
    if (!c) {
        char ka[64], kb[64];
        sc_key_text(a->code, a->name, ka, sizeof(ka));
        sc_key_text(b->code, b->name, kb, sizeof(kb));
        c = stricmp(ka, kb);
    }
    return sc_sort_sc->tsort_desc ? -c : c;
}

/* an AutoText row: its key (the ini spelling when the key has no name),
 * then the text it types */
static void sc_text_row(struct sc_data *sc, int i, char *left, size_t lsize,
                        char *right, size_t rsize)
{
    sc_key_text(sc->texts[i].code, sc->texts[i].name, left, lsize);
    snprintf(right, rsize, "%s", sc->texts[i].text);
}

static void sc_fill_texts(struct sc_data *sc)
{
    if (!sc->dlg) return;
    sc_read_texts(sc);
    sc_fill_list(sc, sc->tlist, sc->ntexts, sc->tsel,
                 KT_KSET_SC_AUTOTEXT_HEAD, sc_cmp_text, sc_text_row);
}

static void sc_show_text(struct sc_data *sc)
{
    char key[64];
    bool row = sc->tsel >= 0 && sc->tsel < sc->ntexts;
    sc_key_text(sc->tcode, row ? sc->texts[sc->tsel].name : NULL, key, sizeof(key));
    dlg_editbox_set(sc->tkey, sc->dlg, sc->tediting && sc->tcode ? key : "");
    dlg_editbox_set(sc->ttext, sc->dlg, row ? sc->texts[sc->tsel].text : "");
    sc_enable(sc, sc->tkey, sc->tediting);
    sc_enable(sc, sc->ttext, sc->tediting);
    sc_enable(sc, sc->tsave, sc->tediting && sc->tcode != 0);
    sc_enable(sc, sc->tdelete, row);
    sc_note(sc, sc->tnote, sc->tediting ? sc->tcode : 0);
}

/* Save (AutoText): the {KEY}=text line, and the key into the list line;
 * a row whose key changed loses its old line first. */
static void sc_save_text(struct sc_data *sc)
{
    char name[64], *text;
    const char *old = NULL;
    int i;
    if (!sc->tediting || !sc->tcode) return;
    if (!ShortcutKeySyntax(sc->tcode, name, sizeof(name))) return;
    text = dlg_editbox_get(sc->ttext, sc->dlg);
    if (!text || !text[0]) { sfree(text); sc_report(KT_KSET_SC_TEXT_EMPTY); return; }
    if (sc_collides(sc, sc->tcode, -1, sc->tsel)) { sfree(text); return; }
    if (!sc_can_write()) { sfree(text); return; }
    if (sc->tsel >= 0 && sc->tsel < sc->ntexts && strcmp(sc->texts[sc->tsel].name, name))
        old = sc->texts[sc->tsel].name;
    if (old) delINI(GetKittyIniFile(), KI_SECTION_SHORTCUTS, old);
    writeINI(GetKittyIniFile(), KI_SECTION_SHORTCUTS, name, text);
    sc_write_list(old, name);
    sfree(text);
    InitShortcuts();
    sc_read_texts(sc);
    sc->tsel = -1;
    for (i = 0; i < sc->ntexts; i++)
        if (!strcmp(sc->texts[i].name, name)) sc->tsel = i;
    sc_fill_texts(sc);
    sc_show_text(sc);
}

static void sc_delete_text(struct sc_data *sc)
{
    if (sc->tsel < 0 || sc->tsel >= sc->ntexts) return;
    if (!sc_can_write()) return;
    delINI(GetKittyIniFile(), KI_SECTION_SHORTCUTS, sc->texts[sc->tsel].name);
    sc_write_list(sc->texts[sc->tsel].name, NULL);
    InitShortcuts();
    sc->tsel = -1; sc->tcode = 0; sc->tediting = false;
    sc_fill_texts(sc);
    sc_show_text(sc);
}

/* ---- the capture field ---- */

/* A modifier key seen as a message: remembered, so a sequence of posted
 * key messages composes like a held key. Returns true for a modifier. */
static bool sc_mod_track(struct sc_data *sc, int vk, bool down)
{
    int v = down ? 1 : 0;
    switch (vk) {
      case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:       sc->mod_shift = v; return true;
      case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: sc->mod_control = v; return true;
      case VK_MENU: case VK_LMENU:                         sc->mod_alt = v; return true;
      case VK_RMENU:                                       sc->mod_alt = v; sc->mod_altgr = v; return true;
      case VK_LWIN: case VK_RWIN:                          sc->mod_win = v; return true;
      default: return false;
    }
}

static void sc_capture(struct sc_data *sc, bool actions, int vk, LPARAM lp)
{
    char key[64];
    int shift = sc->mod_shift || (GetKeyState(VK_SHIFT) & 0x8000);
    int control = sc->mod_control || (GetKeyState(VK_CONTROL) & 0x8000);
    int alt = sc->mod_alt || (GetKeyState(VK_MENU) & 0x8000) || (lp & (1 << 29));
    int altgr = sc->mod_altgr || (GetKeyState(VK_RMENU) & 0x8000);
    int win = sc->mod_win || (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
    int code;
    if ((vk == VK_BACK || vk == VK_DELETE) && !shift && !control && !alt && !win) {
        code = 0;
    } else {
        code = ShortcutKeyCode(vk, shift, control, alt, altgr, win);
        if (!ShortcutKeyText(code, key, sizeof(key)))
            return;                     /* a key with no name: not one */
    }
    if (actions) {
        if (sc->asel < 0) return;
        sc->acode = code;
        sc_show_action(sc);
    } else {
        if (!sc->tediting) return;
        sc->tcode = code;
        sc_key_text(code, NULL, key, sizeof(key));
        dlg_editbox_set(sc->tkey, sc->dlg, code ? key : "");
        sc_enable(sc, sc->tsave, code != 0);
        sc_note(sc, sc->tnote, code);
    }
}

static LRESULT CALLBACK sc_capture_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR ref)
{
    struct sc_data *sc = (struct sc_data *)ref;
    bool actions = (id == SC_SUBCLASS_ACTIONS);
    switch (msg) {
      case WM_NCDESTROY:
        RemoveWindowSubclass(h, sc_capture_proc, id);
        break;
      case WM_GETDLGCODE: {
        /* Every key but Tab and Escape, which keep their dialog meaning. */
        const MSG *m = (const MSG *)lp;
        if (m && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
            (m->wParam == VK_TAB || m->wParam == VK_ESCAPE))
            break;
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
      }
      case WM_KILLFOCUS:
        sc->mod_shift = sc->mod_control = sc->mod_alt = sc->mod_altgr = sc->mod_win = 0;
        break;
      case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;                       /* the field shows key names, not characters */
      case WM_KEYUP: case WM_SYSKEYUP:
        sc_mod_track(sc, (int)wp, false);
        return 0;
      case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int vk = (int)wp;
        if (sc_mod_track(sc, vk, true))
            return 0;                   /* a modifier alone shows nothing */
        if (vk == VK_TAB || vk == VK_ESCAPE)
            break;
        if (vk == VK_F4 && (sc->mod_alt || (GetKeyState(VK_MENU) & 0x8000) || (lp & (1 << 29))))
            break;                      /* Alt+F4 closes the box */
        sc_capture(sc, actions, vk, lp);
        return 0;
      }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* The field as built by controls.c is an ordinary edit: made read-only,
 * given its hint, and subclassed here, once its window exists. */
static void sc_capture_attach(struct sc_data *sc, dlgcontrol *field, UINT_PTR id)
{
    HWND h = sc_hwnd(field), parent;
    wchar_t hint[128];
    char cls[16];
    int base, k;
    if (!h) return;
    /* A labelled edit is two windows, and the first one is the label: the
     * edit is the next id along. */
    parent = GetParent(h);
    base = GetDlgCtrlID(h);
    for (k = 0; k < 3; k++) {
        HWND c = GetDlgItem(parent, base + k);
        if (c && GetClassNameA(c, cls, sizeof(cls)) && !stricmp(cls, "Edit")) { h = c; break; }
    }
    if (k == 3) return;
    SendMessage(h, EM_SETREADONLY, TRUE, 0);
    if (MultiByteToWideChar(CP_ACP, 0, KT_KSET_SC_KEY_HINT, -1, hint, lenof(hint)))
        SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)hint);
    SetWindowSubclass(h, sc_capture_proc, id, (DWORD_PTR)sc);
}

/* A click on a header-row list: the header sorts (a mouse click there)
 * or is stepped off (the keyboard landing on it); a row selects. Returns
 * the row's id, -1 for none. */
static int sc_list_click(struct sc_data *sc, dlgcontrol *list, int *sort_col, bool *sort_desc, int ncols, const int *pct)
{
    HWND h = sc_hwnd(list);
    int idx = dlg_listbox_index(list, sc->dlg);
    if (idx == 0 && h) {
        RECT hr; POINT pt;
        bool mouse = (GetKeyState(VK_LBUTTON) & 0x8000) && GetCursorPos(&pt) &&
            ScreenToClient(h, &pt) &&
            SendMessage(h, LB_GETITEMRECT, 0, (LPARAM)&hr) != LB_ERR &&
            PtInRect(&hr, pt);
        if (mouse) {
            RECT r; int col = 0;
            if (GetClientRect(h, &r) && r.right > r.left) {
                int acc = 0, i;
                for (i = 0; i < ncols - 1; i++) {
                    acc += pct[i];
                    if (pt.x < (r.right - r.left) * acc / 100) break;
                    col = i + 1;
                }
            }
            if (col == *sort_col) *sort_desc = !*sort_desc;
            else { *sort_col = col; *sort_desc = false; }
            return -2;                  /* re-sort */
        }
        if (SendMessage(h, LB_GETCOUNT, 0, 0) > 1) {
            SendMessage(h, LB_SETCURSEL, 1, 0);
            idx = 1;
        } else {
            SendMessage(h, LB_SETCURSEL, (WPARAM)-1, 0);
            return -1;
        }
    }
    if (idx < 0) return -1;
    return dlg_listbox_getid(list, sc->dlg, idx);
}

static const int sc_apct[2] = { 62, 38 };
static const int sc_tpct[2] = { 30, 70 };

static void kitty_sc_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    struct sc_data *sc = (struct sc_data *)ctrl->context.p;
    int which = ctrl->context2.i;
    if (!sc) return;
    sc->dlg = dlg;
    switch (which) {
      case SC_CTX_ALIST:
        if (event == EVENT_REFRESH) {
            sc_fill_actions(sc);
            sc_show_action(sc);
        } else if (event == EVENT_SELCHANGE) {
            int id = sc_list_click(sc, sc->alist, &sc->asort_col, &sc->asort_desc, 2, sc_apct);
            if (id == -2) { sc_fill_actions(sc); break; }
            sc->asel = id;
            sc->acode = id >= 0 ? ShortcutActionValue(id) : 0;
            sc_show_action(sc);
        }
        break;
      case SC_CTX_AKEY:
        if (event == EVENT_REFRESH) sc_capture_attach(sc, ctrl, SC_SUBCLASS_ACTIONS);
        break;
      case SC_CTX_ASAVE:
        if (event == EVENT_ACTION) sc_save_action(sc, sc->acode);
        break;
      case SC_CTX_ADEFAULT:
        if (event == EVENT_ACTION && sc->asel >= 0)
            sc_save_action(sc, ShortcutActionDefault(sc->asel));
        break;
      case SC_CTX_TLIST:
        if (event == EVENT_REFRESH) {
            sc_fill_texts(sc);
            sc_show_text(sc);
        } else if (event == EVENT_SELCHANGE) {
            int id = sc_list_click(sc, sc->tlist, &sc->tsort_col, &sc->tsort_desc, 2, sc_tpct);
            if (id == -2) { sc_fill_texts(sc); break; }
            sc->tsel = id;
            sc->tediting = id >= 0;
            sc->tcode = id >= 0 ? sc->texts[id].code : 0;
            sc_show_text(sc);
        }
        break;
      case SC_CTX_TKEY:
        if (event == EVENT_REFRESH) sc_capture_attach(sc, ctrl, SC_SUBCLASS_TEXTS);
        break;
      case SC_CTX_TTEXT:
        break;
      case SC_CTX_TSAVE:
        if (event == EVENT_ACTION) sc_save_text(sc);
        break;
      case SC_CTX_TDELETE:
        if (event == EVENT_ACTION) sc_delete_text(sc);
        break;
      case SC_CTX_TNEW:
        if (event == EVENT_ACTION) {
            HWND h = sc_hwnd(sc->tlist);
            if (h) SendMessage(h, LB_SETCURSEL, (WPARAM)-1, 0);
            sc->tsel = -1; sc->tcode = 0; sc->tediting = true;
            sc_show_text(sc);
        }
        break;
    }
}

static dlgcontrol *sc_list(struct controlset *s, struct sc_data *sc, int ctx,
                           const int *pct, HelpCtx helpctx)
{
    dlgcontrol *c = ctrl_listbox(s, NULL, NO_SHORTCUT, helpctx,
                                 kitty_sc_handler, P(sc));
    c->context2 = I(ctx);
    c->listbox.height = 4;              /* the floor; each list grows into its leaf's spare height */
    c->listbox.multisel = 0;
    c->listbox.headerrow = true;
    c->listbox.ncols = 2;
    c->listbox.percentages = snewn(2, int);
    c->listbox.percentages[0] = pct[0];
    c->listbox.percentages[1] = pct[1];
    return c;
}

void scb_panel_shortcut_editor(struct controlbox *b, const char *path)
{
    struct sc_data *sc = (struct sc_data *)ctrl_alloc(b, sizeof(*sc));
    struct controlset *s;
    dlgcontrol *c;
    char note[256];

    memset(sc, 0, sizeof(*sc));
    sc->asel = -1; sc->tsel = -1;
    kitty_sc_active = sc;

    s = ctrl_getset(b, path, "actions", KT_KSET_SC_ACTIONS);
    sc->alist = sc_list(s, sc, SC_CTX_ALIST, sc_apct, HELPCTX(kitty_kset_shortcuts));
    ctrl_columns(s, 3, 50, 25, 25);
    c = ctrl_editbox(s, KT_KSET_SC_KEY, NO_SHORTCUT, 72, HELPCTX(kitty_kset_shortcuts),
                     kitty_sc_handler, P(sc), P(NULL));
    c->context2 = I(SC_CTX_AKEY); c->column = 0; sc->akey = c;
    c = ctrl_pushbutton(s, KT_KSET_SC_SAVE, NO_SHORTCUT, HELPCTX(kitty_kset_shortcuts),
                        kitty_sc_handler, P(sc));
    c->context2 = I(SC_CTX_ASAVE); c->column = 1; sc->asave = c;
    c = ctrl_pushbutton(s, KT_KSET_SC_DEFAULT, NO_SHORTCUT, HELPCTX(kitty_kset_shortcuts),
                        kitty_sc_handler, P(sc));
    c->context2 = I(SC_CTX_ADEFAULT); c->column = 2; sc->adefault = c;
    ctrl_columns(s, 1, 100);
    /* Built with the longest text it will carry and two lines reserved
     * either way, so the note set later wraps into the same height; blanked
     * when the panel shows. */
    snprintf(note, sizeof(note), KT_KSET_SC_USERCMD_NOTE, "Ctrl+Shift+W", 23);
    sc->anote = ctrl_text(s, note, HELPCTX(kitty_kset_shortcuts));
    sc->anote->text.lines = 2;

    /* The AutoText entries on a leaf of their own under Shortcuts, with
     * their own help page. The same sc_data serves both leaves. The path
     * is spelt out (not built from `path`) so the help-index audit can
     * attribute the leaf's controls to it. */
    ctrl_settitle(b, KSET_PATH("Keys & Mouse/Shortcuts/AutoText"), KT_KSET_SC_AUTOTEXT_TITLE);
    s = ctrl_getset(b, KSET_PATH("Keys & Mouse/Shortcuts/AutoText"), "autotext", KT_KSET_SC_AUTOTEXT);
    sc->tlist = sc_list(s, sc, SC_CTX_TLIST, sc_tpct, HELPCTX(kitty_kset_autotext));
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_editbox(s, KT_KSET_SC_KEY, NO_SHORTCUT, 72, HELPCTX(kitty_kset_autotext),
                     kitty_sc_handler, P(sc), P(NULL));
    c->context2 = I(SC_CTX_TKEY); c->column = 0; sc->tkey = c;
    ctrl_columns(s, 1, 100);
    c = ctrl_editbox(s, KT_KSET_SC_TEXT, NO_SHORTCUT, 75, HELPCTX(kitty_kset_autotext),
                     kitty_sc_handler, P(sc), P(NULL));
    c->context2 = I(SC_CTX_TTEXT); sc->ttext = c;
    ctrl_columns(s, 3, 34, 33, 33);
    c = ctrl_pushbutton(s, KT_KSET_SC_SAVE, NO_SHORTCUT, HELPCTX(kitty_kset_autotext),
                        kitty_sc_handler, P(sc));
    c->context2 = I(SC_CTX_TSAVE); c->column = 0; sc->tsave = c;
    c = ctrl_pushbutton(s, KT_KSET_SC_DELETE, NO_SHORTCUT, HELPCTX(kitty_kset_autotext),
                        kitty_sc_handler, P(sc));
    c->context2 = I(SC_CTX_TDELETE); c->column = 1; sc->tdelete = c;
    c = ctrl_pushbutton(s, KT_KSET_SC_NEW, NO_SHORTCUT, HELPCTX(kitty_kset_autotext),
                        kitty_sc_handler, P(sc));
    c->context2 = I(SC_CTX_TNEW); c->column = 2; sc->tnew = c;
    ctrl_columns(s, 1, 100);
    /* The send routine's trailing-backslash rule, stated where the text is
     * typed; the help carries the rest. Two lines reserved: the sentence
     * wraps at the panel width. */
    c = ctrl_text(s, KT_KSET_SC_AUTOTEXT_NOTE " " KT_KSET_SC_AUTOTEXT_NOTE_HELP,
                  HELPCTX(kitty_kset_autotext));
    c->text.lines = 2;
    /* The user-command note of the key field, last so that its blank
     * reservation does not open a gap between the Key and Text rows. */
    sc->tnote = ctrl_text(s, note, HELPCTX(kitty_kset_autotext));
    sc->tnote->text.lines = 2;
}

/* ==== The tree's roots: KiTTY++ Settings, Session Panel ================= */

static void scb_panel_kitty_settings(struct controlbox *b, bool midsession)
{
    /* kitty_commun.c's values, which this file has no header for */
    enum { KSET_SAVEMODE_REG = 0, KSET_SAVEMODE_FILE = 1, KSET_SAVEMODE_DIR = 2 };
    static const char *const storage = KSET_PATH("Storage & Backup");
    struct controlset *s;
    char line[1400], buf[4096];
    const char *ini = GetKittyIniFile();
    const char *sav = GetKittySavFile();
    int mode = GetIniFileFlag();
    bool locked = GetReadOnlyFlag() || GetNoKittyFileFlag();

    if (midsession || GetPuttyFlag())
        return;

    ctrl_settitle(b, "Application/KiTTY++ Settings", KT_KSET_TITLE);
    s = ctrl_getset(b, "Application/KiTTY++ Settings", "intro", NULL);
    ctrl_text(s, KT_KSET_INTRO_WHOLE, HELPCTX(kitty_settings_tree));
    ctrl_text(s, KT_KSET_INTRO_WHERE, HELPCTX(kitty_settings_tree));

    ctrl_settitle(b, storage, KT_KSET_STORAGE_TITLE);

    /* The status: store, file, and whether writes are possible. ctrl_text
     * copies its string, so a line built here need not outlive the call. */
    s = ctrl_getset(b, storage, "status", KT_KSET_STORAGE_THIS_KITTY);
    if (mode == KSET_SAVEMODE_DIR)
        snprintf(line, sizeof(line), KT_KSET_STORAGE_STORE_FOLDER,
                 ConfigDirectory ? ConfigDirectory : "");
    else if (mode == KSET_SAVEMODE_FILE)
        snprintf(line, sizeof(line), "%s", KT_KSET_STORAGE_STORE_SAV);
    else
        snprintf(line, sizeof(line), KT_KSET_STORAGE_STORE_REGISTRY,
                 kitty_registry_base());
    ctrl_text(s, line, HELPCTX(kitty_storage));
    if (GetNoKittyFileFlag() || !ini || !ini[0])
        snprintf(line, sizeof(line), "%s", KT_KSET_STORAGE_INI_NONE);
    else
        snprintf(line, sizeof(line), KT_KSET_STORAGE_INI, ini);
    ctrl_text(s, line, HELPCTX(kitty_storage));
    if (GetReadOnlyFlag())
        ctrl_text(s, KT_KSET_STORAGE_READONLY, HELPCTX(kitty_storage));
    if (GetNoKittyFileFlag())
        ctrl_text(s, KT_KSET_STORAGE_NOCONF, HELPCTX(kitty_storage));

    /* The backups: the ONE count this store mode reads - a folder store
     * keeps portablebackupcount copies of itself, the registry modes keep
     * savbackupcount .sav exports - editable when a write can land, shown as
     * text when not. The other mode's key would be a field that does
     * nothing here, which is the confusion this tree exists to remove. */
    s = ctrl_getset(b, storage, "backups", KT_KSET_BACKUPS);
    {
        const bool dir = (mode == KSET_SAVEMODE_DIR);
        const char *key = dir ? "portablebackupcount" : "savbackupcount";
        if (!locked) {
            ctrl_editbox(s, dir ? KT_KSET_BACKUPS_DIR : KT_KSET_BACKUPS_REG,
                         NO_SHORTCUT, 20, HELPCTX(kitty_storage),
                         kitty_kset_backupcount_handler, P((char *)key), ED_STR);
        } else {
            snprintf(line, sizeof(line),
                     dir ? KT_KSET_BACKUPS_DIR_SHOWN : KT_KSET_BACKUPS_REG_SHOWN,
                     kitty_kset_backupcount(key));
            ctrl_text(s, line, HELPCTX(kitty_storage));
        }
    }
    /* Where they go. A folder store copies itself under its own Backups
     * folder; the registry modes export the hive to timestamped files
     * beside the sav path. */
    if (mode == KSET_SAVEMODE_DIR)
        snprintf(line, sizeof(line), KT_KSET_BACKUPS_DIR_PATH,
                 ConfigDirectory ? ConfigDirectory : "");
    else
        snprintf(line, sizeof(line), KT_KSET_BACKUPS_SAV_PATH,
                 sav ? sav : "");
    ctrl_text(s, line, HELPCTX(kitty_storage));

    /* restrictacl: display only, on purpose. It is the one key read from
     * kitty.ini alone so that a stale registry value can never cancel a
     * hardening switch, and a control here would write to the registry. */
    s = ctrl_getset(b, storage, "hardening", KT_KSET_HARDENING);
    ctrl_text(s, restricted_acl() ? KT_KSET_RESTRICTACL_ON
                                  : KT_KSET_RESTRICTACL_OFF,
              HELPCTX(kitty_storage));
    ctrl_text(s, KT_KSET_RESTRICTACL_NOTE, HELPCTX(kitty_storage));

    /* The rest of what the store is: which hive, which session-file
     * extension, which legacy salt - display only, each would move or
     * strand data if edited here - and the folder store's password policy,
     * which is read at save time and so needs no running value. */
    s = ctrl_getset(b, storage, "identity", NULL);
    buf[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_KICLASSNAME, buf, sizeof(buf));
    snprintf(line, sizeof(line), KT_KSET_STORAGE_KICLASS,
             buf[0] ? buf : KT_KSET_STORAGE_KICLASS_DEFAULT);
    ctrl_text(s, line, HELPCTX(kitty_storage));
    buf[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_FILEEXTENSION, buf, sizeof(buf));
    snprintf(line, sizeof(line), KT_KSET_STORAGE_FILEEXT, buf[0] ? buf : ".ktx");
    ctrl_text(s, line, HELPCTX(kitty_storage));
    buf[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_CRYPTSALT, buf, sizeof(buf));
    snprintf(line, sizeof(line), KT_KSET_STORAGE_CRYPTSALT, buf[0] ? buf : "1");
    ctrl_text(s, line, HELPCTX(kitty_storage));
    if (mode == KSET_SAVEMODE_DIR) {
        s = ctrl_getset(b, storage, "passwords", KT_KSET_STORAGE_PWGROUP);
        if (!locked) {
            KSET_DROPLIST(s, KT_KSET_STORAGE_PWPROT, KI_PORTABLEPASSWORDPROTECTION, kitty_storage);
            ctrl_text(s, KT_KSET_STORAGE_PWPROT_NOTE, HELPCTX(kitty_storage));
            KSET_CHECKBOX(s, KT_KSET_STORAGE_WARNLEGACY, KI_WARNLEGACYPASSWORDUPGRADE, kitty_storage);
        }
    }

    /* No footer here by hand: the Application builder adds the
     * saved-as-you-change-them line to every panel with an editable control,
     * which when nothing can be written this panel has none of. */

    /* Storage & Backup > KiTTY.ini: the file itself, read-only. */
    scb_panel_iniview(b, ini);

    scb_panel_kitty_settings_leaves(b);
}

/*
 * Application > Config Window > Session panel (was "Session parameter").
 *
 * The saved-session list and what it offers, kept apart from the window that
 * happens to draw it: Config Window is about the window - its colours, its
 * size, its tree - and these are about sessions. They were together while
 * there was only one panel to put them on.
 */
static void scb_panel_session_parameter(struct controlbox *b, bool midsession)
{
    struct controlset *s;

    if (midsession || GetPuttyFlag())
        return;

    ctrl_settitle(b, "Application/Config Window/Session Panel", KT_SESSION_PARAMETER_THE_SESSION_LIST);

    s = ctrl_getset(b, "Application/Config Window/Session Panel", "list", KT_SESSION_PARAMETER_THE_LIST);
    ctrl_editbox(s, KT_SESSION_PARAMETER_LENGTH_IN_ROWS_7, NO_SHORTCUT, 30,
                 HELPCTX(kitty_folders), kitty_cfgwin_num_handler, P(KI_CONFIGBOX_HEIGHT),
                 ED_STR);
    ctrl_checkbox(s, KT_SESSION_PARAMETER_SHOW_DEFAULT_SETTINGS,
                  NO_SHORTCUT, HELPCTX(kitty_folders),
                  kitty_cfgwin_flag_handler, P(KI_CONFIGBOX_DEFAULTSETTINGS));
    ctrl_text(s, KT_SESSION_PARAMETER_QUICK_CONNECT_NEEDS_IT_LOADING, HELPCTX(kitty_folders));
    ctrl_checkbox(s, KT_SESSION_PARAMETER_SHOW_FOLDERS_AS_ROWS_NOT,
                  NO_SHORTCUT, HELPCTX(kitty_folders),
                  kitty_cfgwin_flag_handler, P(KI_CONFIGBOX_FOLDERNAVIGATION));
    ctrl_checkbox(s, KT_SESSION_PARAMETER_SEARCH_THE_LIST_AS_YOU,
                  NO_SHORTCUT, HELPCTX(kitty_folders),
                  kitty_cfgwin_flag_handler, P(KI_CONFIGBOX_FILTER));

    s = ctrl_getset(b, "Application/Config Window/Session Panel", "opening", KT_SESSION_PARAMETER_OPENING);
    ctrl_checkbox(s, KT_SESSION_PARAMETER_OPEN_ON_THE_LAST_USED,
                  NO_SHORTCUT, HELPCTX(kitty_quickconnect),
                  kitty_cfgwin_flag_handler, P(KI_CONFIGBOX_LOADLASTSESSION));
    ctrl_text(s, KT_SESSION_PARAMETER_QUICK_CONNECT_STARTS_EVERY_KITTY,
              HELPCTX(kitty_quickconnect));
    ctrl_droplist(s, KT_SESSION_PARAMETER_DOUBLE_CLICK_A_SESSION, NO_SHORTCUT, 55,
                  HELPCTX(kitty_quickconnect), kitty_cfgwin_dblclick_handler, P(NULL));

    s = ctrl_getset(b, "Application/Config Window/Session Panel", "proxy", KT_SESSION_PARAMETER_PROXY);
    ctrl_droplist(s, KT_SESSION_PARAMETER_SHOW_THE_PROXY_CHOOSER, NO_SHORTCUT, 55,
                  HELPCTX(kitty_named_proxies), kitty_cfgwin_proxysel_handler, P(NULL));
    /* Said here because "Never" does more than hide one droplist. */
    ctrl_text(s, KT_SESSION_PARAMETER_NEVER_ALSO_HIDES_THE_EDIT, HELPCTX(kitty_named_proxies));
}

/*
 * The APPLICATION tab's panels (KiTTY).
 *
 * These are settings about the program, not about the session in front of
 * you, and they are reached through the Session | Application tabs above the
 * category tree rather than by a root in the session tree. Their paths carry
 * an "Application/" prefix, which is how the tree build tells the two tabs
 * apart; the prefix is stripped when the items are inserted, so the tree shows
 * "Updates" rather than "Application/Updates" under a tab already called
 * Application.
 *
 * "Check for updates" is the [KiTTY] checkupdate key
 * (kitty_check_update_enabled in kitty_win.c); the CONF_check_update_startup
 * option only remains so that saved sessions carrying CheckUpdateStartup
 * still load.
 */
/*
 * Application > Migration: the sessions in the old hives, and Import.
 *
 * The list is not the saved-session list. That one answers "what can I open",
 * so it hides a foreign session behind a session of ours with the same name
 * and disappears entirely when the show-old-sessions switch is off - and both
 * of those are sessions this panel still has to be able to import.
 */
struct import_data {
    dlgcontrol *listbox;
    dlgcontrol *banner;
    struct kitty_foreign_list *found;
};

/* ==== Migration, the ini and store moves, the panel footers ============= */

static void import_say(struct import_data *im, dlgparam *dlg, const char *what)
{
    if (im && im->banner)
        dlg_label_change(im->banner, dlg, what);
}

static void kitty_import_list_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    struct import_data *im = (struct import_data *)ctrl->context.p;
    int i;

    if (event != EVENT_REFRESH || !im)
        return;
    /* Re-read the hives on every refresh: an import adds no row (the source is
     * left alone) but a delete from the session list removes one. */
    kitty_foreign_list_free(im->found);
    im->found = kitty_foreign_sessions();

    dlg_update_start(ctrl, dlg);
    dlg_listbox_clear(ctrl, dlg);
    for (i = 0; i < im->found->n; i++) {
        char *row = dupprintf("%s\t%s", im->found->items[i].name,
                              kitty_foreign_hive_label(im->found->items[i].hive));
        dlg_listbox_addwithid(ctrl, dlg, row, i);
        sfree(row);
    }
    dlg_update_done(ctrl, dlg);
}

static void kitty_import_action_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    struct import_data *im = (struct import_data *)ctrl->context.p;
    struct kitty_namelist dropped;
    strbuf *names;
    int i, done = 0, failed = 0;

    if (event != EVENT_ACTION || !im || !im->found || !im->listbox)
        return;

    memset(&dropped, 0, sizeof(dropped));
    names = strbuf_new();
    for (i = 0; i < im->found->n; i++) {
        char *got;
        int which;
        if (!dlg_listbox_issel(im->listbox, dlg, i))
            continue;
        /* The row carries the index it was filled from. Asking the row rather
         * than trusting the loop counter is what keeps this honest if the two
         * ever stop being filled in one pass - and an out-of-range answer
         * (LB_ERR) is then a row to skip rather than a wild read. */
        which = dlg_listbox_getid(im->listbox, dlg, i);
        if (which < 0 || which >= im->found->n)
            continue;
        got = kitty_import_foreign_session(im->found->items[which].name,
                                           im->found->items[which].hive,
                                           &dropped);
        if (!got) {
            failed++;
            continue;
        }
        if (done)
            put_dataz(names, ", ");
        put_dataz(names, got);
        sfree(got);
        done++;
    }

    if (!done && !failed) {
        strbuf_free(names);
        kitty_namelist_clear(&dropped);
        import_say(im, dlg, KT_MIG_IMP_NOSEL);
        return;
    }

    /* The saved-session list gains the copies at once, rather than at the next
     * configuration window. */
    if (done && session_filter_ssd) {
        get_sesslist(&session_filter_ssd->sesslist, false);
        get_sesslist(&session_filter_ssd->sesslist, true);
        kitty_session_folder_cache_clear();
        dlg_refresh(session_filter_ssd->listbox, dlg);
    }

    /*
     * ONE summary for the whole import, not a message per session: the names
     * the copies got (they differ from the originals where the name was taken)
     * and, once, the settings that were left behind.
     */
    {
        strbuf *msg = strbuf_new();
        if (done)
            put_fmt(msg, KT_MIG_BOX_OK, done, done == 1 ? "" : "s", names->s);
        if (failed)
            put_fmt(msg, KT_MIG_BOX_FAILED, failed, failed == 1 ? "" : "s");
        if (dropped.n) {
            char *list = kitty_namelist_join(&dropped, ", ");
            put_fmt(msg, KT_MIG_BOX_DROPPED, list);
            put_dataz(msg, KT_MIG_BOX_SEEHELP);
            sfree(list);
        }
        MessageBoxA(kitty_cfg_modal_owner(), msg->s, KT_MIG_BOX_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        strbuf_free(msg);
    }

    import_say(im, dlg, done ? KT_MIG_IMP_DONE : KT_MIG_IMP_NONE);
    strbuf_free(names);
    kitty_namelist_clear(&dropped);
}

/*
 * The line every Application panel ends with, EXCEPT the two that are not
 * true of: Named Proxies and the CA editor both hold an edit until Save.
 * One helper rather than a line copied into each panel, so it can be reworded
 * or dropped in one place.
 */
/* Application > Migration: the whole-store Export all / Import all pair
 * (context 0 = export, 1 = import). The work lives in kitty_bridge.c; the
 * import refresh reaches the Session panel's list through the same ssd the
 * button distribution uses. */
/* Application > Migration > KiTTY storage: the two store moves of
 * kitty_storemove.c. Taking a folder store in changes the session list. */
static void kitty_inimig_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    (void)data;
    if (event != EVENT_ACTION)
        return;
    if (ctrl->context.i == 0) {
        kitty_make_portable_copy(GetActiveWindow());
    } else {
        struct sessionsaver_data *ssd = kitty_session_ssd;
        kitty_take_folder_store(GetActiveWindow());
        if (ssd && ssd->listbox) {
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            kitty_session_folder_cache_clear();
            dlg_refresh(ssd->listbox, dlg);
        }
        kitty_notify_launcher_sessions_changed();
    }
}

static void kitty_storexfer_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    (void)data;
    if (event != EVENT_ACTION)
        return;
    if (ctrl->context.i == 0) {
        kitty_export_all_sessions(GetActiveWindow());
    } else {
        struct sessionsaver_data *ssd = kitty_session_ssd;
        kitty_import_sessions(GetActiveWindow());
        if (ssd && ssd->listbox) {
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            kitty_session_folder_cache_clear();
            dlg_refresh(ssd->listbox, dlg);
        }
        kitty_notify_launcher_sessions_changed();
    }
}

/* The "saved as you change them" footers, pinned to the BOTTOM of their
 * panels the way the Proxy pre-set loader is: recorded here as they are
 * created, moved by kitty_config_footer_pin after each layout and by
 * kitty_config_pin_bottoms on every resize. */
/* One entry per Application panel that carries the footer. Sized from the
 * box when it is built (one slot per control set is a safe upper bound):
 * a fixed array of 12 once dropped the thirteenth panel's entry silently,
 * and that footer sat wherever the layout had left it. */
static struct app_footer_pin {
    const char *path;
    dlgcontrol *ctrl;
    /* recorded from the fresh layout: the text's top, the frame's top
     * relative to it, and the pair's lowest edge - all in host client
     * coordinates, all the pin's later moves are computed from these */
    int natural_y, natural_box_dy, natural_lowest, have_natural;
} *app_footers = NULL;
static int n_app_footers = 0, app_footers_cap = 0;

static void kitty_footer_pin_one(struct app_footer_pin *f)
{
    HWND host = kitty_cfg_panel_host, w, boxw = NULL;
    RECT hostr, gr, br;
    POINT p;
    int y, id;
    char cls[16];

    if (!host || !f->ctrl || !(w = kitty_cfg_ctrl_hwnd(f->ctrl)))
        return;
    if (!GetWindowRect(w, &gr))
        return;
    /* The set's GROUP BOX travels with its text, or pinning leaves an empty
     * frame at the layout position - which is exactly what the first version
     * did. The box's id sits directly below the text's, and only a real
     * group box is accepted for it. */
    id = GetDlgCtrlID(w);
    if (id > 0 && (boxw = GetDlgItem(host, id - 1)) != NULL) {
        if (!GetClassNameA(boxw, cls, sizeof(cls)) || stricmp(cls, "Button") ||
            !(GetWindowLong(boxw, GWL_STYLE) & BS_GROUPBOX) ||
            !GetWindowRect(boxw, &br))
            boxw = NULL;
    }
    if (!f->have_natural) {
        POINT t = { 0, gr.top }, lb = { 0, gr.bottom };
        ScreenToClient(host, &t);
        ScreenToClient(host, &lb);
        f->natural_y = t.y;
        f->natural_box_dy = 0;
        f->natural_lowest = lb.y;
        if (boxw) {
            POINT bt = { 0, br.top }, bb = { 0, br.bottom };
            ScreenToClient(host, &bt);
            ScreenToClient(host, &bb);
            f->natural_box_dy = bt.y - t.y;
            if (bb.y > f->natural_lowest)
                f->natural_lowest = bb.y;
        }
        f->have_natural = 1;
    }
    GetClientRect(host, &hostr);
    /* One delta for text and frame together, anchored on the pair's LOWEST
     * edge - the frame closes below the text, and it is the frame that must
     * not cross the panel's bottom. Never negative: the pin only ever hands
     * back what extra height the panel has, it does not push the layout up. */
    y = (hostr.bottom - 4) - f->natural_lowest;
    if (y < 0)
        y = 0;
    y += f->natural_y;
    if (boxw) {
        p.x = br.left; p.y = 0;
        ScreenToClient(host, &p);
        SetWindowPos(boxw, NULL, p.x, y + f->natural_box_dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    p.x = gr.left; p.y = 0;
    ScreenToClient(host, &p);
    SetWindowPos(w, NULL, p.x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* After one panel's layout: fresh natural position, then pin. */
void kitty_config_footer_pin(const char *path)
{
    for (int i = 0; i < n_app_footers; i++) {
        if (!strcmp(app_footers[i].path, path)) {
            app_footers[i].have_natural = 0;
            kitty_footer_pin_one(&app_footers[i]);
            return;
        }
    }
}

/* On every resize: everything pinned to the panel area's bottom edge. */
void kitty_config_pin_bottoms(void)
{
    for (int i = 0; i < n_app_footers; i++)
        kitty_footer_pin_one(&app_footers[i]);
}

void scb_app_footer(struct controlbox *b, const char *path)
{
    struct controlset *s;
    bool exists = false;

    /* Only onto a panel that was actually built. ctrl_getset() would create
     * one, and a panel that exists only to carry the footer would appear in
     * the tree as a leaf with nothing on it. */
    for (size_t i = 0; i < b->nctrlsets; i++)
        if (!strcmp(b->ctrlsets[i]->pathname, path)) { exists = true; break; }
    if (!exists)
        return;

    s = ctrl_getset(b, path, "footer", NULL);
    {
        dlgcontrol *c = ctrl_text(s, KT_APP_SAVED_LIVE,
                                  HELPCTX(kitty_folders));
        if (n_app_footers < app_footers_cap) {
            app_footers[n_app_footers].path = path;
            app_footers[n_app_footers].ctrl = c;
            app_footers[n_app_footers].have_natural = 0;
            n_app_footers++;
        }
    }
}

/* ---- Migration > old KiTTY Folders ------------------------------------ */

static int migf_refreshing = 0;        /* dlg_editbox_set fires VALCHANGE */

/* The folder typed or chosen as the target, cleaned the way the Session
 * panel cleans a new folder name; KiTTYimport when the field is empty. */
static char *migf_target_folder(struct migf_data *m, dlgparam *dlg)
{
    char *t = m->target ? dlg_editbox_get(m->target, dlg) : NULL;
    char folder[256];
    snprintf(folder, sizeof(folder), "%s", (t && *t) ? t : KT_MIGF_DEFAULT_FOLDER);
    sfree(t);
    CleanFolderName(folder);
    if (!folder[0])
        snprintf(folder, sizeof(folder), "%s", KT_MIGF_DEFAULT_FOLDER);
    return dupstr(folder);
}

/* Session | Path | Target | State, one row per file found. The target name
 * is worked out here, row by row, exactly as the import will hand it out -
 * names this list already claimed count as taken - so the column says what
 * WILL happen. */
static void migf_fill_list(struct migf_data *m, dlgparam *dlg)
{
    struct kitty_namelist taken;
    int i;

    if (!m->listbox)
        return;
    memset(&taken, 0, sizeof(taken));
    dlg_update_start(m->listbox, dlg);
    dlg_listbox_clear(m->listbox, dlg);
    /* Row 0 is the column header (listbox.headerrow): id -1, so every
     * loop over selected rows skips it. */
    dlg_listbox_addwithid(m->listbox, dlg, KT_MIGF_COL_HEAD, -1);
    for (i = 0; m->found && i < m->found->n; i++) {
        struct kitty_folder_scan_item *it = &m->found->items[i];
        char *target = kitty_import_folder_target_name(it->name, it->folder, &taken);
        const char *state =
            it->state == KFS_UNREADABLE    ? KT_MIGF_ST_UNREADABLE :
            it->state == KFS_PASSWORD      ? KT_MIGF_ST_PASSWORD :
            it->state == KFS_PASSWORD_MPW  ? KT_MIGF_ST_MPW :
            (target && strcmp(target, it->name)) ? KT_MIGF_ST_EXISTS : KT_MIGF_ST_READY;
        /* Path LAST, and relative to the scanned folder: the tab stops align
         * the three short columns, and the one of any length runs off the
         * end where it pushes nothing. */
        const char *rel = it->path;
        size_t rootlen = strlen(m->root);
        if (rootlen && !_strnicmp(rel, m->root, rootlen) && rel[rootlen] == '\\')
            rel += rootlen + 1;
        char *row = dupprintf("%s\t%s\t%s\t%s\t%s", it->name, state,
                              it->folder, target ? target : "", rel);
        dlg_listbox_addwithid(m->listbox, dlg, row, i);
        sfree(row);
        if (target && it->state != KFS_UNREADABLE)
            kitty_namelist_add(&taken, target);
        sfree(target);
    }
    dlg_update_done(m->listbox, dlg);
    kitty_namelist_clear(&taken);
}

static void migf_say(struct migf_data *m, dlgparam *dlg, const char *what)
{
    if (m->banner)
        dlg_label_change(m->banner, dlg, what);
}

static void kitty_migf_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    struct migf_data *m = (struct migf_data *)ctrl->context.p;
    int which = ctrl->context2.i;      /* 0 folder box, 1 browse, 2 scan,
                                          3 target combo, 4 assign, 5 list,
                                          6 import */
    if (!m)
        return;

    switch (which) {
      case 0:                          /* the folder to scan */
        if (event == EVENT_REFRESH) {
            migf_refreshing = 1;
            dlg_editbox_set(ctrl, dlg, m->root);
            migf_refreshing = 0;
        } else if (event == EVENT_VALCHANGE && !migf_refreshing) {
            char *s = dlg_editbox_get(ctrl, dlg);
            snprintf(m->root, sizeof(m->root), "%s", s);
            sfree(s);
        }
        break;

      case 1:                          /* Browse... */
        if (event == EVENT_ACTION) {
            char dir[MAX_PATH * 2];
            if (OpenDirName(dlg->hwnd, dir) && dir[0]) {
                snprintf(m->root, sizeof(m->root), "%s", dir);
                if (m->folderbox)
                    dlg_refresh(m->folderbox, dlg);
            }
        }
        break;

      case 2:                          /* Scan */
        if (event == EVENT_ACTION) {
            char *folder;
            strbuf *line;
            if (!m->root[0]) { migf_say(m, dlg, KT_MIGF_NO_FOLDER); break; }
            if (!existdirectory(m->root)) { migf_say(m, dlg, KT_MIGF_NOT_A_DIR); break; }
            folder = migf_target_folder(m, dlg);
            kitty_folder_scan_free(m->found);
            m->found = kitty_scan_folder_store(m->root, folder);
            sfree(folder);
            migf_fill_list(m, dlg);
            line = strbuf_new();
            if (m->found->n)
                put_fmt(line, KT_MIGF_FOUND, m->found->n, m->found->n == 1 ? "" : "s",
                        m->found->dirs_seen, m->found->dirs_seen == 1 ? "" : "s");
            else
                put_dataz(line, KT_MIGF_FOUND_NONE);
            if (m->found->hit_depth)
                put_fmt(line, KT_MIGF_LIMIT_DEPTH, KFS_MAX_DEPTH);
            if (m->found->hit_count)
                put_fmt(line, KT_MIGF_LIMIT_COUNT, KFS_MAX_FILES);
            migf_say(m, dlg, line->s);
            strbuf_free(line);
        }
        break;

      case 3:                          /* Import into folder: */
        if (event == EVENT_REFRESH) {
            int i;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            dlg_listbox_add(ctrl, dlg, KT_MIGF_DEFAULT_FOLDER);
            for (i = 0; FolderList && FolderList[i]; i++)
                if (FolderList[i][0] && strcmp(FolderList[i], KT_MIGF_DEFAULT_FOLDER))
                    dlg_listbox_add(ctrl, dlg, FolderList[i]);
            /* What the user typed or picked survives a refresh; only a box
             * that never had a value shows the default. */
            if (!m->folder[0])
                snprintf(m->folder, sizeof(m->folder), "%s", KT_MIGF_DEFAULT_FOLDER);
            migf_refreshing = 1;
            dlg_editbox_set(ctrl, dlg, m->folder);
            migf_refreshing = 0;
            dlg_update_done(ctrl, dlg);
        } else if ((event == EVENT_VALCHANGE || event == EVENT_SELCHANGE) &&
                   !migf_refreshing) {
            char *s = dlg_editbox_get(ctrl, dlg);
            snprintf(m->folder, sizeof(m->folder), "%s", s);
            sfree(s);
        }
        break;

      case 4:                          /* Assign to selected */
        if (event == EVENT_ACTION) {
            char *folder;
            int i, n = 0;
            if (!m->found || !m->listbox) { migf_say(m, dlg, KT_MIGF_NOSEL); break; }
            folder = migf_target_folder(m, dlg);
            /* Rows = the header + one per file: the loop runs over ROWS. */
            for (i = 0; i <= m->found->n; i++) {
                int id;
                if (!dlg_listbox_issel(m->listbox, dlg, i))
                    continue;
                id = dlg_listbox_getid(m->listbox, dlg, i);
                if (id < 0 || id >= m->found->n)
                    continue;
                sfree(m->found->items[id].folder);
                m->found->items[id].folder = dupstr(folder);
                n++;
            }
            sfree(folder);
            migf_fill_list(m, dlg);
            migf_say(m, dlg, n ? KT_MIGF_ASSIGNED : KT_MIGF_NOSEL);
        }
        break;

      case 5:                          /* the list */
        if (event == EVENT_REFRESH) {
            migf_fill_list(m, dlg);
        } else if (event == EVENT_SELCHANGE) {
            /* The header row cannot be selected: a click on it (or a
             * select-all) is undone at once. */
            HWND h = kitty_cfg_ctrl_hwnd(ctrl);
            if (h && SendMessage(h, LB_GETSEL, 0, 0) > 0)
                SendMessage(h, LB_SETSEL, FALSE, 0);
        }
        break;

      case 6:                          /* Import selected */
        if (event == EVENT_ACTION) {
            struct kitty_namelist dropped, taken;
            strbuf *names;
            char *store_pass = NULL;   /* the source store's master password, or NULL */
            int i, done = 0, failed = 0, nopw = 0;

            if (!m->found || !m->listbox) { migf_say(m, dlg, KT_MIGF_NOSEL); break; }
            memset(&dropped, 0, sizeof(dropped));
            memset(&taken, 0, sizeof(taken));
            /*
             * The source store's master password, asked ONCE per import and
             * only when a selected row needs it. The import's own asking and
             * the import's own key: our store's unlock is not touched. Three
             * tries against the first such file; cancel or three misses and
             * those sessions import without their passwords.
             */
            {
                const char *probe = NULL;
                for (i = 0; i <= m->found->n && !probe; i++) {
                    int id;
                    if (!dlg_listbox_issel(m->listbox, dlg, i))
                        continue;
                    id = dlg_listbox_getid(m->listbox, dlg, i);
                    if (id >= 0 && id < m->found->n &&
                        m->found->items[id].state == KFS_PASSWORD_MPW)
                        probe = m->found->items[id].path;
                }
                store_pass = NULL;
                for (int tries = 0; probe && tries < 3 && !store_pass; tries++) {
                    char *p = kitty_mpw_gui_ask_import(dlg->hwnd, KT_MIGF_MPW_PROMPT);
                    if (!p)
                        break;                       /* cancelled: import without */
                    if (kitty_import_store_pass_fits(probe, p))
                        store_pass = p;
                    else {
                        memset(p, 0, strlen(p));
                        free(p);
                    }
                }
            }
            names = strbuf_new();
            for (i = 0; i <= m->found->n; i++) {   /* rows: header + files */
                struct kitty_folder_scan_item *it;
                char *target;
                bool lost = false;
                int id;
                if (!dlg_listbox_issel(m->listbox, dlg, i))
                    continue;
                id = dlg_listbox_getid(m->listbox, dlg, i);
                if (id < 0 || id >= m->found->n)
                    continue;
                it = &m->found->items[id];
                if (it->state == KFS_UNREADABLE) { failed++; continue; }
                target = kitty_import_folder_target_name(it->name, it->folder, &taken);
                if (!target || !kitty_import_file_session(it->path, target, it->folder,
                                                          store_pass, &dropped, &lost)) {
                    sfree(target);
                    failed++;
                    continue;
                }
                kitty_namelist_add(&taken, target);
                if (done)
                    put_dataz(names, ", ");
                put_dataz(names, target);
                sfree(target);
                done++;
                if (lost)
                    nopw++;
            }
            if (!done && !failed) {
                strbuf_free(names);
                kitty_namelist_clear(&dropped);
                kitty_namelist_clear(&taken);
                migf_say(m, dlg, KT_MIGF_NOSEL);
                break;
            }
            if (done) {
                /* The Session panel is out of date now: the list has new rows,
                 * the folder combo (or the folder rows) a new folder, and the
                 * name box may name a session that no longer sits where it
                 * did. The same three refreshes a folder change makes; the
                 * launcher is told as well. */
                struct sessionsaver_data *ssd = kitty_session_ssd;
                InitFolderList();
                if (ssd && ssd->listbox) {
                    get_sesslist(&ssd->sesslist, false);
                    get_sesslist(&ssd->sesslist, true);
                    kitty_session_folder_cache_clear();
                    if (ssd->editbox)    dlg_refresh(ssd->editbox, dlg);
                    if (ssd->folderlist) dlg_refresh(ssd->folderlist, dlg);
                    dlg_refresh(ssd->listbox, dlg);
                }
                kitty_notify_launcher_sessions_changed();
                if (m->target)
                    dlg_refresh(m->target, dlg);
            }
            {
                strbuf *msg = strbuf_new();
                if (done)
                    put_fmt(msg, KT_MIGF_BOX_OK, done, done == 1 ? "" : "s", names->s);
                if (nopw)
                    put_fmt(msg, KT_MIGF_BOX_NOPW, nopw);
                if (failed)
                    put_fmt(msg, KT_MIGF_BOX_FAILED, failed, failed == 1 ? "" : "s");
                if (dropped.n) {
                    char *list = kitty_namelist_join(&dropped, ", ");
                    put_fmt(msg, KT_MIG_BOX_DROPPED, list);
                    put_dataz(msg, KT_MIG_BOX_SEEHELP);
                    sfree(list);
                }
                MessageBoxA(kitty_cfg_modal_owner(), msg->s, KT_MIGF_BOX_TITLE,
                            MB_OK | MB_ICONINFORMATION);
                strbuf_free(msg);
            }
            migf_fill_list(m, dlg);    /* "already exists" is true now */
            migf_say(m, dlg, done ? KT_MIGF_DONE : KT_MIGF_NONE);
            strbuf_free(names);
            kitty_namelist_clear(&dropped);
            kitty_namelist_clear(&taken);
            if (store_pass) { memset(store_pass, 0, strlen(store_pass)); free(store_pass); }
        }
        break;
    }
}

static void scb_panel_folder_import(struct controlbox *b)
{
    static const char *const path = KCFG_PATH_OLD_FOLDERS;
    struct migf_data *m;
    struct controlset *s;
    dlgcontrol *c;

    m = (struct migf_data *)ctrl_alloc(b, sizeof(*m));
    memset(m, 0, sizeof(*m));
    kitty_migf_active = m;

    ctrl_settitle(b, path, KT_MIGF_TITLE);

    s = ctrl_getset(b, path, "scan", KT_MIGF_SCAN_GROUP);
    ctrl_text(s, KT_MIGF_SCAN_INTRO, HELPCTX(kitty_import_folders));
    m->folderbox = ctrl_editbox(s, KT_MIGF_FOLDER, NO_SHORTCUT, 100,
                                HELPCTX(kitty_import_folders),
                                kitty_migf_handler, P(m), I(0));
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_pushbutton(s, KT_MIGF_BROWSE, NO_SHORTCUT, HELPCTX(kitty_import_folders),
                        kitty_migf_handler, P(m));
    c->context2 = I(1); c->column = 0;
    c = ctrl_pushbutton(s, KT_MIGF_SCAN, NO_SHORTCUT, HELPCTX(kitty_import_folders),
                        kitty_migf_handler, P(m));
    c->context2 = I(2); c->column = 1;
    ctrl_columns(s, 1, 100);

    s = ctrl_getset(b, path, "target", KT_MIGF_TARGET_GROUP);
    m->target = ctrl_combobox(s, KT_MIGF_TARGET, NO_SHORTCUT, 60,
                              HELPCTX(kitty_import_folders),
                              kitty_migf_handler, P(m), I(3));
    ctrl_text(s, KT_MIGF_TARGET_NOTE, HELPCTX(kitty_import_folders));
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_pushbutton(s, KT_MIGF_ASSIGN, NO_SHORTCUT, HELPCTX(kitty_import_folders),
                        kitty_migf_handler, P(m));
    c->context2 = I(4); c->column = 1;
    ctrl_columns(s, 1, 100);

    s = ctrl_getset(b, path, "list", KT_MIGF_LIST_GROUP);
    m->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT, HELPCTX(kitty_import_folders),
                              kitty_migf_handler, P(m));
    m->listbox->context2 = I(5);
    m->listbox->listbox.height = 6;    /* the floor; the fill hook grows it */
    m->listbox->listbox.multisel = 1;
    m->listbox->listbox.headerrow = true;   /* row 0 = the column header */
    m->listbox->listbox.ncols = 5;
    m->listbox->listbox.percentages = snewn(5, int);
    m->listbox->listbox.percentages[0] = 20;   /* Session */
    m->listbox->listbox.percentages[1] = 20;   /* State */
    m->listbox->listbox.percentages[2] = 14;   /* Folder */
    m->listbox->listbox.percentages[3] = 22;   /* Saved as */
    m->listbox->listbox.percentages[4] = 24;   /* Path */
    /* The result line and the button share one row, directly under the
     * list: a line of its own put the button a full row further down. */
    ctrl_columns(s, 2, 60, 40);
    m->banner = ctrl_text(s, " ", HELPCTX(kitty_import_folders));
    m->banner->column = 0;
    c = ctrl_pushbutton(s, KT_MIGF_IMPORT, NO_SHORTCUT, HELPCTX(kitty_import_folders),
                        kitty_migf_handler, P(m));
    c->context2 = I(6); c->column = 1;
    ctrl_columns(s, 1, 100);
}

/* ==== The Application tab's root panels ================================= */

void scb_panel_application(struct controlbox *b, bool midsession)
{
    struct controlset *s;

    if (midsession || GetPuttyFlag())
        return;                        /* no application tab mid-session */
    {
        /* [ConfigBox] applicationsettings=no: no Application tab at all. The
         * tab strip already degrades to one tab when no Application/ path
         * exists (it is how the stock variants behave), and the jumps to
         * those panels check the strip before going. */
        if (!GetConfigBoxApplicationSettingsFlag())
            return;
    }

    /* Fresh box, fresh footer registrations. At the top, before anything is
     * built: a reset placed beside the footer pass at the end once wiped a
     * registration a panel had made while building itself - the line was
     * created, never pinned, and sat wherever the layout had left it.
     * The table is sized once the panels exist, in the footer pass. */
    n_app_footers = 0;

    /* The named-proxy editor, which used to be a pop-up window. */
    kitty_proxy_build_panel(b);

    scb_panel_config_window(b, midsession);
    scb_panel_session_parameter(b, midsession);
    scb_panel_kitty_settings(b, midsession);   /* includes Transfers & Tools */
    scb_panel_security(b, midsession);

    /*
      * Migration: moving sessions between installations. The whole-store
      * Export all / Import all pair leads, because it concerns every install;
      * the foreign-hive blocks below it appear only on registry-backed
      * installs whose old hives actually hold sessions - a portable store has
      * no foreign hive to reveal, and an empty switch explains nothing.
      */
    {
        ctrl_settitle(b, "Application/Migration", KT_MIG_TITLE);
        s = ctrl_getset(b, "Application/Migration", "storexfer",
                        KT_MIG_STORE_GROUP);
        ctrl_columns(s, 2, 50, 50);
        {
            dlgcontrol *c2;
            c2 = ctrl_pushbutton(s, KT_SESSION_EXPORT_ALL, NO_SHORTCUT,
                                 HELPCTX(session_saved),
                                 kitty_storexfer_handler, I(0));
            c2->column = 0;
            c2 = ctrl_pushbutton(s, KT_SESSION_IMPORT_ALL, NO_SHORTCUT,
                                 HELPCTX(session_saved),
                                 kitty_storexfer_handler, I(1));
            c2->column = 1;
        }
        ctrl_columns(s, 1, 100);

        if (GetIniFileFlag() == 0 /* SAVEMODE_REG */ &&
            kitty_has_foreign_sessions()) {
            s = ctrl_getset(b, "Application/Migration", "foreign",
                            KT_MIG_OLD_GROUP);
            ctrl_text(s, KT_MIG_OLD_INTRO, HELPCTX(kitty_import_sessions));
            ctrl_checkbox(s, KT_MIG_SHOW_BOX, NO_SHORTCUT, HELPCTX(kitty_import_sessions),
                          kitty_showforeign_handler, P(NULL));
            /* The switch also gates loading by name (open_settings_r), so a
             * hidden old session is not found by -load either - said here,
             * where the switch is. */
            ctrl_text(s, KT_MIG_SHOW_NOTE, HELPCTX(kitty_import_sessions));

            {
                struct import_data *im = (struct import_data *)
                    ctrl_alloc(b, sizeof(struct import_data));
                memset(im, 0, sizeof(*im));

                s = ctrl_getset(b, "Application/Migration", "import",
                                KT_MIG_IMP_GROUP);
                ctrl_text(s, KT_MIG_IMP_INTRO, HELPCTX(kitty_import_sessions));
                im->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                           HELPCTX(kitty_import_sessions),
                                           kitty_import_list_handler, P(im));
                im->listbox->listbox.height = 6;
                im->listbox->listbox.multisel = 1;
                im->listbox->listbox.ncols = 2;
                im->listbox->listbox.percentages = snewn(2, int);
                im->listbox->listbox.percentages[0] = 70;
                im->listbox->listbox.percentages[1] = 30;
                /* Button at its natural width against the panel's right
                 * border - a full-width action bar it is not. The banner
                 * starts blank; it exists to carry the import's RESULT
                 * (import_say), not an instruction nobody needed. */
                {
                    dlgcontrol *bc;
                    ctrl_columns(s, 2, 55, 45);
                    bc = ctrl_pushbutton(s, KT_MIG_IMP_BUTTON, NO_SHORTCUT,
                                         HELPCTX(kitty_import_sessions),
                                         kitty_import_action_handler, P(im));
                    bc->column = 1;
                    ctrl_columns(s, 1, 100);
                }
                im->banner = ctrl_text(s, " ",
                                       HELPCTX(kitty_import_sessions));
            }
        }
    }

    /* Application > Migration > KiTTY storage: registry <-> folder
     * store, both ways. "Take ... into this registry" makes no sense for a
     * copy that already runs from a folder, so only the registry-mode copy
     * offers it. */
    {
        const char *p = "Application/Migration/KiTTY storage";
        dlgcontrol *bc;
        ctrl_settitle(b, p, KT_INIMIG_TITLE);
        s = ctrl_getset(b, p, "copy", KT_INIMIG_OUT_GROUP);
        ctrl_text(s, KT_INIMIG_OUT_INTRO, HELPCTX(kitty_ini_migration));
        ctrl_columns(s, 2, 55, 45);
        bc = ctrl_pushbutton(s, KT_INIMIG_OUT_BUTTON, NO_SHORTCUT,
                             HELPCTX(kitty_ini_migration),
                             kitty_inimig_handler, I(0));
        bc->column = 1;
        ctrl_columns(s, 1, 100);
        if (GetIniFileFlag() != 2 /* SAVEMODE_DIR */) {
            s = ctrl_getset(b, p, "take", KT_INIMIG_IN_GROUP);
            ctrl_text(s, KT_INIMIG_IN_INTRO, HELPCTX(kitty_ini_migration));
            ctrl_text(s, KT_INIMIG_IN_NOTE, HELPCTX(kitty_ini_migration));
            ctrl_columns(s, 2, 55, 45);
            bc = ctrl_pushbutton(s, KT_INIMIG_IN_BUTTON, NO_SHORTCUT,
                                 HELPCTX(kitty_ini_migration),
                                 kitty_inimig_handler, I(1));
            bc->column = 1;
            ctrl_columns(s, 1, 100);
        }
    }

    /* Migration > old KiTTY Folders: sessions kept in files. */
    scb_panel_folder_import(b);

    ctrl_settitle(b, "Application/Updates", KT_UPDATES_KEEPING_KITTY_UP_TO_DATE);
    s = ctrl_getset(b, "Application/Updates", "check", KT_UPDATES_UPDATE_CHECK);
    /* On startup, check for a newer release and show a one-line notice in the
     * terminal when a session opens. Edits kitty.ini, not the session. */
    ctrl_checkbox(s, KT_UPDATES_CHECK_FOR_UPDATES_WHEN_KITTY, NO_SHORTCUT,
                  HELPCTX(kitty_updater), kitty_checkupdate_global_handler, P(NULL));
    /* The check on demand, next to the switch that decides whether it happens
     * by itself. */
    ctrl_pushbutton(s, KT_UPDATES_CHECK_FOR_UPDATES_NOW, NO_SHORTCUT,
                    HELPCTX(kitty_updater), checkupdate_button_handler, P(NULL));

    /*
     * Last, so the line ends each panel: an application setting takes effect
     * where it is changed, which a session panel's OK/Apply does not lead
     * anyone to expect. Named Proxies and the CA editor are deliberately not
     * in this list - they hold an edit until Save and each says so itself.
     */
    /*
     * Which panels get it is not a list but a RULE: any Application panel
     * with a SETTING on it - an edit box, check box, radio, list, file or
     * font chooser; not text, not a push button (an action, not a value),
     * not a layout pseudo-control. A panel of text alone
     * (an intro page, or Storage & Backup when nothing can be written), or
     * of buttons alone (Migration in a folder store), would carry a claim
     * about nothing. The two panels that hold an edit until Save are named
     * out, since for them the line would be false rather than empty.
     */
    {
        static const char *const holds_until_save[] = {
            "Application/Named Proxies",
            "Application/Security/Certificate Authorities",
        };
        /* One slot per control set is more than one per panel, and the box
         * that decides the count is the one being built - no constant to
         * outgrow. The previous box's table goes with the previous box. */
        sfree(app_footers);
        app_footers_cap = (int)b->nctrlsets;
        app_footers = snewn(app_footers_cap > 0 ? app_footers_cap : 1,
                            struct app_footer_pin);
        n_app_footers = 0;
        for (size_t i = 0; i < b->nctrlsets; i++) {
            const char *path = b->ctrlsets[i]->pathname;
            bool editable = false, seen = false, excluded = false;
            if (strncmp(path, "Application/", 12))
                continue;
            /* one visit per panel: skip a path already handled */
            for (size_t j = 0; j < i; j++)
                if (!strcmp(b->ctrlsets[j]->pathname, path)) { seen = true; break; }
            if (seen)
                continue;
            for (size_t k = 0; k < lenof(holds_until_save); k++)
                if (!strcmp(path, holds_until_save[k])) { excluded = true; break; }
            /* The kitty.ini VIEW has a droplist and a box, but it stores
             * nothing: "saved as you change them" would be false there. */
            if (!strcmp(path, KCFG_PATH_INIVIEW))
                excluded = true;
            /* The folder import: its fields drive an action, they store nothing. */
            if (!strcmp(path, KCFG_PATH_OLD_FOLDERS))
                excluded = true;
            /* The host-key list shows the store; Delete acts at once and says so. */
            if (!strcmp(path, KCFG_PATH_HOSTKEYS))
                excluded = true;
            if (excluded)
                continue;
            for (size_t j = i; j < b->nctrlsets && !editable; j++) {
                struct controlset *cs = b->ctrlsets[j];
                if (strcmp(cs->pathname, path))
                    continue;
                for (size_t c = 0; c < cs->ncontrols; c++) {
                    /* A positive list: text, buttons AND the layout
                     * pseudo-controls (ctrl_columns, tab-order hints) are
                     * not settings. */
                    switch (cs->ctrls[c]->type) {
                      case CTRL_EDITBOX: case CTRL_RADIO: case CTRL_CHECKBOX:
                      case CTRL_LISTBOX: case CTRL_FILESELECT:
                      case CTRL_FONTSELECT:
                        editable = true;
                        break;
                      default:
                        break;
                    }
                    if (editable)
                        break;
                }
            }
            if (editable)
                scb_app_footer(b, path);
        }
    }
}
