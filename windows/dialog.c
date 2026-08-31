/*
 * dialog.c - dialogs for PuTTY(tel), including the configuration dialog.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>
#include <time.h>

#include "putty.h"
#include "ssh.h"
#include "putty-rc.h"
#include "win-gui-seat.h"
#include "storage.h"
#include "dialog.h"
#include "licence.h"
#include "../kitty/kitty_theme.h"   /* KiTTY: dark mode for the dialogs */
#include "../kitty/kitty_anchor.h"  /* KiTTY: the resizable configuration box */

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef MSVC4
#define TVINSERTSTRUCT  TV_INSERTSTRUCT
#define TVITEM          TV_ITEM
#define ICON_BIG        1
#endif

typedef struct PortableDialogStuff {
    /*
     * These are the various bits of data required to handle a dialog
     * box that's been built up from the cross-platform dialog.c
     * system.
     */

    /* The 'controlbox' that was returned from the portable setup function */
    struct controlbox *ctrlbox;

    /* The dlgparam that's passed to all the runtime dlg_* functions.
     * Declared as an array of 1 so it's convenient to pass it as a pointer. */
    struct dlgparam dp[1];

    /*
     * Collections of instantiated controls. There can be more than
     * one of these, because sometimes you want to destroy and
     * recreate a subset of them - e.g. when switching panes in the
     * main PuTTY config box, you delete and recreate _most_ of the
     * controls, but not the OK and Cancel buttons at the bottom.
     */
    size_t nctrltrees;
    struct winctrls *ctrltrees;

    /*
     * Flag indicating whether the dialog box has been initialised.
     * Used to suppresss spurious firing of message handlers during
     * setup.
     */
    bool initialised;
} PortableDialogStuff;

/*
 * Initialise a PortableDialogStuff, before launching the dialog box.
 */
static PortableDialogStuff *pds_new(size_t nctrltrees)
{
    PortableDialogStuff *pds = snew(PortableDialogStuff);
    memset(pds, 0, sizeof(*pds));

    pds->ctrlbox = ctrl_new_box();

    dp_init(pds->dp);

    pds->nctrltrees = nctrltrees;
    pds->ctrltrees = snewn(pds->nctrltrees, struct winctrls);
    for (size_t i = 0; i < pds->nctrltrees; i++) {
        winctrl_init(&pds->ctrltrees[i]);
        dp_add_tree(pds->dp, &pds->ctrltrees[i]);
    /* The control box describes every panel whether or not its windows have
     * been built, so it is what a whole-session check has to work from. */
    kitty_conf_ctrlbox_is(pds->ctrlbox);
    }

    pds->dp->errtitle = dupprintf("%s Error", appname);

    pds->initialised = false;

    return pds;
}

/*
 * Settings a loaded session held that no option in the box can represent.
 *
 * A session is just text on disk: a hand edit, a file written by a newer
 * build, a truncated write, and a setting ends up holding a number that is
 * not one of the values it can take. Upstream asserts on that and the whole
 * application goes down over one bad line. Nothing about a bad value in a
 * file justifies losing the session the user was working in, so instead the
 * value is replaced with the default, the replacement is recorded, and the
 * load says what it had to change.
 *
 * The check lives here rather than in the loader because this is where the
 * legal values are KNOWN - they are the buttons of the group. The loader sees
 * only a name and a number and has no way to tell a wrong one from a right
 * one. The consequence is that a session loaded from the command line, which
 * never builds the panels, is not checked until its configuration box is
 * opened.
 */
int kitty_conf_default_int(int key);

static char *kitty_invalid_settings = NULL;   /* comma-separated, or NULL */
static int kitty_invalid_count = 0;
static char *kitty_invalid_session = NULL;    /* whose session it was */

void kitty_conf_invalid_note(const char *what)
{
    char *old = kitty_invalid_settings;
    if (!what || !*what)
        what = "(unnamed setting)";
    kitty_invalid_count++;
    if (kitty_invalid_count > 12)     /* a wholly corrupt file, not a list */
        return;
    kitty_invalid_settings = old ? dupcat(old, ", ", what) : dupstr(what);
    sfree(old);
}

void kitty_conf_invalid_reset(void)
{
    sfree(kitty_invalid_settings);
    kitty_invalid_settings = NULL;
    kitty_invalid_count = 0;
}

void kitty_conf_invalid_session_is(const char *name)
{
    sfree(kitty_invalid_session);
    kitty_invalid_session = dupstr(name ? name : "");
}

void kitty_conf_invalid_report(dlgparam *dlg, const char *session)
{
    char *msg;
    if (!kitty_invalid_settings)
        return;
    if (!session)
        session = kitty_invalid_session;
    msg = dupprintf(
        "Session \"%s\" contains %d setting%s this version cannot "
        "represent:\n\n%s%s\n\nThe default%s been used instead. Saving "
        "the session will store the corrected value%s.",
        session ? session : "", kitty_invalid_count,
        kitty_invalid_count == 1 ? "" : "s", kitty_invalid_settings,
        kitty_invalid_count > 12 ? ", ..." : "",
        kitty_invalid_count == 1 ? " has" : "s have",
        kitty_invalid_count == 1 ? "" : "s");
    dlg_error_msg(dlg, msg);
    sfree(msg);
    kitty_conf_invalid_reset();
}

/*
 * Check a whole loaded session against what the controls can actually hold.
 *
 * This walks the abstract control box rather than the built panels, and that
 * is the entire point: with the panel cache, a panel's WINDOWS do not exist
 * until it is first shown, so a handler-based check only ever sees the panels
 * somebody happened to visit. A session with a bad value on an unvisited
 * panel would then be written straight back out, still bad, having been
 * neither corrected nor reported. The control box, in contrast, describes
 * every panel from the moment the box is built, buttons and all.
 *
 * The fallback in conf_radiobutton_handler stays where it is. This pass is
 * the one that finds everything; that one is the guarantee that nothing gets
 * through even so.
 */
static struct controlbox *kitty_conf_ctrlbox = NULL;

void kitty_conf_ctrlbox_is(struct controlbox *b)
{
    kitty_conf_ctrlbox = b;
}

void kitty_conf_validate(Conf *conf)
{
    size_t si, ci;
    int button;

    if (!kitty_conf_ctrlbox || !conf)
        return;

    for (si = 0; si < kitty_conf_ctrlbox->nctrlsets; si++) {
        struct controlset *s = kitty_conf_ctrlbox->ctrlsets[si];
        for (ci = 0; ci < s->ncontrols; ci++) {
            dlgcontrol *ctrl = s->ctrls[ci];
            int val;

            if (ctrl->type != CTRL_RADIO ||
                ctrl->handler != conf_radiobutton_handler)
                continue;

            val = conf_get_int(conf, ctrl->context.i);
            for (button = 0; button < ctrl->radio.nbuttons; button++)
                if (val == ctrl->radio.buttondata[button].i)
                    break;
            if (button < ctrl->radio.nbuttons)
                continue;                      /* a value it can hold */

            button = 0;
            {
                int def = kitty_conf_default_int(ctrl->context.i);
                int b;
                for (b = 0; b < ctrl->radio.nbuttons; b++)
                    if (def == ctrl->radio.buttondata[b].i) {
                        button = b;
                        break;
                    }
            }
            kitty_conf_invalid_note(ctrl->label);
            conf_set_int(conf, ctrl->context.i,
                         ctrl->radio.buttondata[button].i);
        }
    }
}

/*
 * The value this setting would have in a session that never mentioned it.
 * do_defaults(NULL) is the application's own idea of a default, so a site
 * that has customised "Default Settings" gets ITS value rather than the
 * built-in one, which is what a user expects the word to mean. Built once and
 * kept: it is read on an error path and does not change while the program
 * runs.
 */
int kitty_conf_default_int(int key)
{
    static Conf *defaults = NULL;
    if (!defaults) {
        defaults = conf_new();
        do_defaults(NULL, defaults);
    }
    return conf_get_int(defaults, key);
}

static void pds_free(PortableDialogStuff *pds)
{
    /* Before the box goes, or the session check would be left pointing at
     * freed memory - and it is called from a load, which can happen in the
     * next configuration box this process opens. */
    kitty_conf_ctrlbox_is(NULL);
    ctrl_free_box(pds->ctrlbox);

    dp_cleanup(pds->dp);

    for (size_t i = 0; i < pds->nctrltrees; i++)
        winctrl_cleanup(&pds->ctrltrees[i]);
    sfree(pds->ctrltrees);

    sfree(pds);
}

/* The active config-box panel's own help topic, for the WM_HELP fallback
 * below; NULL when hwnd is not the config box or the panel carries none.
 * Implemented after the panel cache it reads. */
static const char *kitty_cfg_panel_helpctx(HWND hwnd);

static INT_PTR pds_default_dlgproc(PortableDialogStuff *pds, HWND hwnd,
                                   UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_LBUTTONUP:
        /*
         * Button release should trigger WM_OK if there was a
         * previous double click on the host CA list.
         */
        ReleaseCapture();
        if (pds->dp->ended)
            ShinyEndDialog(hwnd, pds->dp->endresult ? 1 : 0);
        break;
      case WM_COMMAND:
      case WM_DRAWITEM:
      default: {                       /* also handle drag list msg here */
        /*
         * Only process WM_COMMAND once the dialog is fully formed.
         */
        int ret;
        if (pds->initialised) {
            ret = winctrl_handle_command(pds->dp, msg, wParam, lParam);
            if (pds->dp->ended && GetCapture() != hwnd)
                ShinyEndDialog(hwnd, pds->dp->endresult ? 1 : 0);
        } else
            ret = 0;
        return ret;
      }
      case WM_HELP:
        if (!winctrl_context_help(pds->dp,
                                  hwnd, ((LPHELPINFO)lParam)->iCtrlId)) {
            /* KiTTY: a spot with no context of its own opens the manual
             * anyway - at the current panel's topic when the config box is
             * the window asking. A beep answered "where is the help" with a
             * noise. */
            const char *ctx = kitty_cfg_panel_helpctx(hwnd);
            launch_help(hwnd, ctx);
        }
        break;
      case WM_CLOSE:
        quit_help(hwnd);
        ShinyEndDialog(hwnd, 0);
        return 0;

        /* Grrr Explorer will maximize Dialogs! */
      case WM_SIZE:
        if (wParam == SIZE_MAXIMIZED)
            force_normal(hwnd);
        return 0;

    }
    return 0;
}

static void pds_initdialog_start(PortableDialogStuff *pds, HWND hwnd)
{
    pds->dp->hwnd = hwnd;

    if (pds->dp->wintitle)     /* apply override title, if provided */
        SetWindowText(hwnd, pds->dp->wintitle);

    /* The portable dialog system generally includes the ability to
     * handle context help for particular controls. Enable the
     * relevant window styles if we have a help file available. */
    if (has_help()) {
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, style | WS_EX_CONTEXTHELP);
    } else {
        /* If not, and if the dialog template provided a top-level
         * Help button, delete it */
        HWND item = GetDlgItem(hwnd, IDC_HELPBTN);
        if (item)
            DestroyWindow(item);
    }
}

/*
 * Create the panelfuls of controls in the configuration box.
 */
static void pds_create_controls(
    PortableDialogStuff *pds, size_t which_tree, int base_id,
    int left, int right, int top, char *path)
{
    struct ctlpos cp;

    ctlposinit(&cp, pds->dp->hwnd, left, right, top);

    for (int index = -1; (index = ctrl_find_path(
                              pds->ctrlbox, path, index)) >= 0 ;) {
        struct controlset *s = pds->ctrlbox->ctrlsets[index];
        winctrl_layout(pds->dp, &pds->ctrltrees[which_tree], &cp, s, &base_id);
    }
}

static void pds_initdialog_finish(PortableDialogStuff *pds)
{
    /*
     * Set focus into the first available control in ctrltree #0,
     * which the caller was expected to set up to be the one
     * containing the dialog controls likely to be used first.
     */
    struct winctrl *c;
    for (int i = 0; (c = winctrl_findbyindex(&pds->ctrltrees[0], i)) != NULL;
         i++) {
        if (c->ctrl) {
            dlg_set_focus(c->ctrl, pds->dp);
            break;
        }
    }

    /*
     * Now we've finished creating our initial set of controls,
     * it's safe to actually show the window without risking setup
     * flicker.
     */
    ShowWindow(pds->dp->hwnd, SW_SHOWNORMAL);

    pds->initialised = true;
}

#define LOGEVENT_INITIAL_MAX 128
#define LOGEVENT_CIRCULAR_MAX 128

static char *events_initial[LOGEVENT_INITIAL_MAX];
static char *events_circular[LOGEVENT_CIRCULAR_MAX];
static int ninitial = 0, ncircular = 0, circular_first = 0;

#define PRINTER_DISABLED_STRING "None (printing disabled)"

void force_normal(HWND hwnd)
{
    static bool recurse = false;

    WINDOWPLACEMENT wp;

    if (recurse)
        return;
    recurse = true;

    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
        wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd, &wp);
    }
    recurse = false;
}

static char *getevent(int i)
{
    if (i < ninitial)
        return events_initial[i];
    if ((i -= ninitial) < ncircular)
        return events_circular[(circular_first + i) % LOGEVENT_CIRCULAR_MAX];
    return NULL;
}

static HWND logbox;
HWND event_log_window(void) { return logbox; }

static void update_logbox_horizontal_extent(HWND logbox)
{
    /* Find the width of every entry in the current Event Log, and set
     * the horizontal scrollbar so that you can scroll right to see
     * all of them. */
    HWND listbox = GetDlgItem(logbox, IDN_LIST);
    HDC hdc = GetDC(listbox);
    int count = SendMessage(listbox, LB_GETCOUNT, 0, 0);
    strbuf *sb = strbuf_new();
    WPARAM maxwidth = 0;
    for (int i = 0; i < count; i++) {
        size_t len = SendMessage(listbox, LB_GETTEXTLEN, i, 0);
        strbuf_clear(sb);
        SendMessage(listbox, LB_GETTEXT, i, (LPARAM)strbuf_append(sb, len));
        SIZE size;
        if (GetTextExtentPoint(hdc, sb->s, sb->len, &size)) {
            if (maxwidth < size.cx)
                maxwidth = size.cx;
        }
    }
    strbuf_free(sb);
    ReleaseDC(listbox, hdc);

    /*
     * This doesn't seem to set _exactly_ the right width. If I scroll
     * the scrollbar right as far as it will go, I find I end up with
     * a bit of extra space on the right of the longest piece of text.
     * I don't know why that is.
     *
     * Testing with debug(), GetTextExtentPoint seems to be correctly
     * returning the exact width in pixels of each log entry's text.
     * And the docs for LB_SETHORIZONTALEXTENT say that it wants a
     * width in pixels too. So I don't think it can be the usual
     * problem of confusing logical dialog units and pixels. (In any
     * case, on the high-DPI display where I tested, the difference
     * between those is more than a factor of 2, whereas I'm seeing a
     * discrepancy of only about 10%.)
     */
    SendMessage(listbox, LB_SETHORIZONTALEXTENT, maxwidth, 0);
}

/* KiTTY: the Event Log is resizable (WS_THICKFRAME in the template); fit the
 * listbox to the dialog with the buttons centred underneath. Sizes derive
 * from the buttons' current (template+DPI-scaled) metrics, so this stays
 * correct at any DPI. */
static void logbox_layout(HWND hwnd)
{
    RECT rc, rb;
    HWND list = GetDlgItem(hwnd, IDN_LIST);
    HWND btnok = GetDlgItem(hwnd, IDOK);
    HWND btncopy = GetDlgItem(hwnd, IDN_COPY);
    HWND btnclear = GetDlgItem(hwnd, IDN_CLEAR);
    int bw, bh, m, btop, gap, left;
    if (!list || !btnok || !btncopy || !btnclear) return;
    GetClientRect(hwnd, &rc);
    GetWindowRect(btnok, &rb);
    bw = rb.right - rb.left; bh = rb.bottom - rb.top;
    m = bh / 3;
    btop = rc.bottom - bh - m;
    MoveWindow(list, m, m, rc.right - 2*m, btop - 2*m, true);
    gap = bw / 4;
    left = (rc.right - (3*bw + 2*gap)) / 2;
    /* KiTTY: Close, Copy, Clear - in that order left to right, matching the
     * order the controls appear in the template, because THAT is the tab
     * order. Laid out the other way round (Clear leftmost, Close rightmost)
     * Tab and the arrow keys walked the buttons right to left, which is what
     * a user hitting Tab from Close actually reported. Fixing it here rather
     * than by reordering the template keeps the two useful properties of the
     * current order: the initial focus is the default button, and it is not
     * the destructive one. */
    MoveWindow(btnok, left, btop, bw, bh, true);
    MoveWindow(btncopy, left + bw + gap, btop, bw, bh, true);
    MoveWindow(btnclear, left + 2*(bw + gap), btop, bw, bh, true);
}

static INT_PTR CALLBACK LogProc(HWND hwnd, UINT msg,
                                WPARAM wParam, LPARAM lParam)
{
    int i;

    switch (msg) {
      case WM_INITDIALOG: {
        char *str = dupprintf("%s Event Log", appname);
        SetWindowText(hwnd, str);
        sfree(str);

        static int tabs[4] = { 78, 108 };
        SendDlgItemMessage(hwnd, IDN_LIST, LB_SETTABSTOPS, 2,
                           (LPARAM) tabs);

        for (i = 0; i < ninitial; i++)
            SendDlgItemMessage(hwnd, IDN_LIST, LB_ADDSTRING,
                               0, (LPARAM) events_initial[i]);
        for (i = 0; i < ncircular; i++)
            SendDlgItemMessage(hwnd, IDN_LIST, LB_ADDSTRING,
                               0, (LPARAM) events_circular[(circular_first + i) % LOGEVENT_CIRCULAR_MAX]);
        update_logbox_horizontal_extent(hwnd);

        logbox_layout(hwnd);
        return 1;
      }
      case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            logbox_layout(hwnd);
        return 0;
      case WM_GETMINMAXINFO: {
        /* Don't let it shrink below a readable minimum. */
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 320;
        mmi->ptMinTrackSize.y = 200;
        return 0;
      }
      case WM_VKEYTOITEM:
        /* KiTTY: the listbox has LBS_WANTKEYBOARDINPUT so Ctrl+A can select
         * every line and Ctrl+C can trigger Copy (which, with nothing
         * selected, copies the whole log). -2 = fully handled, -1 = default. */
        if (LOWORD(wParam) == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendDlgItemMessage(hwnd, IDN_LIST, LB_SETSEL, true, (LPARAM)-1);
            return -2;
        }
        if (LOWORD(wParam) == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessage(hwnd, WM_COMMAND,
                        MAKEWPARAM(IDN_COPY, BN_CLICKED),
                        (LPARAM)GetDlgItem(hwnd, IDN_COPY));
            return -2;
        }
        return -1;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            logbox = NULL;
            SetActiveWindow(GetParent(hwnd));
            DestroyWindow(hwnd);
            return 0;
          case IDN_CLEAR:
            /* KiTTY: empty the log. Useful before reproducing something, so
             * what follows is only what the reproduction produced. The stored
             * strings are freed here; the log then fills from scratch. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                for (i = 0; i < ninitial; i++) {
                    sfree(events_initial[i]);
                    events_initial[i] = NULL;
                }
                for (i = 0; i < LOGEVENT_CIRCULAR_MAX; i++) {
                    sfree(events_circular[i]);
                    events_circular[i] = NULL;
                }
                ninitial = ncircular = circular_first = 0;
                SendDlgItemMessage(hwnd, IDN_LIST, LB_RESETCONTENT, 0, 0);
                update_logbox_horizontal_extent(hwnd);
            }
            return 0;
          case IDN_COPY:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                int selcount;
                int *selitems;
                static const unsigned char sel_nl[] = SEL_NL;
                int total = ninitial + ncircular;
                selcount = SendDlgItemMessage(hwnd, IDN_LIST,
                                              LB_GETSELCOUNT, 0, 0);
                if (selcount <= 0) {
                    /* KiTTY: nothing selected -> copy the WHOLE Event Log instead
                     * of just beeping, so the common "grab the entire log" case
                     * needs no manual select-all. (Selecting lines still copies
                     * only those.) */
                    if (total <= 0) { MessageBeep(0); break; }
                    strbuf *sb = strbuf_new();
                    for (i = 0; i < total; i++) {
                        char *q = getevent(i);
                        if (!q) continue;
                        put_datapl(sb, ptrlen_from_asciz(q));
                        put_data(sb, sel_nl, sizeof(sel_nl));
                    }
                    if (sb->len > 0)
                        write_aclip(hwnd, CLIP_SYSTEM, sb->s, sb->len);
                    strbuf_free(sb);
                    break;
                }

                selitems = snewn(selcount, int);
                if (selitems) {
                    int count = SendDlgItemMessage(hwnd, IDN_LIST,
                                                   LB_GETSELITEMS,
                                                   selcount,
                                                   (LPARAM) selitems);

                    if (count == 0) {  /* can't copy zero stuff */
                        MessageBeep(0);
                        sfree(selitems);
                        break;
                    }

                    strbuf *sb = strbuf_new();
                    for (int i = 0; i < count; i++) {
                        char *q = getevent(selitems[i]);
                        put_datapl(sb, ptrlen_from_asciz(q));
                        put_data(sb, sel_nl, sizeof(sel_nl));
                    }
                    write_aclip(hwnd, CLIP_SYSTEM, sb->s, sb->len);
                    strbuf_free(sb);
                    sfree(selitems);

                    for (i = 0; i < total; i++)
                        SendDlgItemMessage(hwnd, IDN_LIST, LB_SETSEL,
                                           false, i);
                }
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        logbox = NULL;
        SetActiveWindow(GetParent(hwnd));
        DestroyWindow(hwnd);
        return 0;
    }
    return 0;
}

static INT_PTR CALLBACK LicenceProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        char *str = dupprintf("%s Licence", appname);
        SetWindowText(hwnd, str);
        sfree(str);
        SetDlgItemText(hwnd, IDA_TEXT, LICENCE_TEXT("\r\n\r\n"));
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 1);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

/* kitty_auxpos.c: DPI/monitor-safe aux-window placement + position memory. */
void kitty_auxpos_apply(HWND dlg, const char *key, HWND anchor, int near_tray);
void kitty_auxpos_save(HWND dlg, const char *key);

/* KiTTY: the About box is now a single NON-modal (modeless) window. */
static HWND kitty_about_dlg = NULL;
static void kitty_show_about_modeless(HWND owner);

static INT_PTR CALLBACK AboutProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    char *str;

    switch (msg) {
      case WM_INITDIALOG: {
        str = dupprintf("About %s", appname);
        SetWindowText(hwnd, str);
        sfree(str);
        char *buildinfo_text = buildinfo("\r\n");
#ifdef MOD_NETDEBUG
        /* Debug build marker so this exe is distinguishable from a normal build. */
        const char *netdbg =
            "\r\n*** NETDEBUG BUILD - event log is teed to "
            "%USERPROFILE%\\kitty_netdebug.log ***";
#else
        const char *netdbg = "";
#endif
#ifdef KITTY_TEST_BUILD_LABEL
        const char *testbuild = "\r\n*** TEST BUILD: " KITTY_TEST_BUILD_LABEL " ***";
#else
        const char *testbuild = "";
#endif
        /* KiTTY: say when this process runs with the restricted ACL. Failing to
         * APPLY it is loud (fail-closed), but the setting never being read is
         * silent - kitty.ini has four candidate locations - so a user can
         * believe they are hardened when they are not. Reports the STATE
         * (restricted_acl()), so -restrict-acl on a shortcut shows here too and
         * this cannot drift from what is actually in force. It means "this
         * process's ACL is locked down", nothing broader. */
        const char *aclnote = restricted_acl() ?
            "\r\n\r\nRunning with a restricted process ACL: other programs "
            "under your account cannot open this process." : "";
        /* far2l attribution is unconditional: dialog.c compiles into a shared
         * lib that does not carry the per-target MOD_FAR2L define, and KiTTY
         * always ships the far2l extensions, so the credit is always accurate. */
        /* UTF-8 source: real (c) (\xc2\xa9) and em-dash (\xe2\x80\x94) rather than
         * CP1252 bytes, set as Unicode below so they render on any system codepage. */
        char *text = dupprintf(
            "%s\r\n\r\n%s%s%s%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s"
            "\r\n\r\n%s",
            appname, ver, netdbg, testbuild, aclnote, buildinfo_text,
            "This PuTTY 0.85 port \xc2\xa9 KAPPER NETWORK-COMMUNICATIONS GmbH "
            "\xe2\x80\x94 https://github.com/hknet/KiTTY",
            "KiTTY \xc2\xa9 2007-2013 Cyril Dupont \xe2\x80\x94 https://www.9bis.net/kitty/",
            "Based on PuTTY \xc2\xa9 " SHORT_COPYRIGHT_DETAILS ". All rights reserved.",
            "far2l terminal extensions from putty4far2l "
            "(Ivan Sorokin, unxed, Ivan Shatsky); far2l \xe2\x80\x94 elfmz.",
            /* Session > Scripting is the RuTTY patch, carried forward like the
             * far2l extensions above - and unconditional for the same reason
             * given there: this file compiles into a shared lib that carries no
             * per-target MOD_ define, and every KiTTY build ships the engine. */
            "Session scripting from the RuTTY patch \xc2\xa9 2013-2014 "
            "Ernst Dijk.");
        sfree(buildinfo_text);
        {
            int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
            wchar_t *wtext = snewn(wn > 0 ? wn : 1, wchar_t);
            if (wn > 0 && MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wn) > 0)
                SetDlgItemTextW(hwnd, IDA_TEXT, wtext);
            else
                SetDlgItemText(hwnd, IDA_TEXT, text);  /* fallback */
            sfree(wtext);
        }
        MakeDlgItemBorderless(hwnd, IDA_TEXT);
        sfree(text);
        /* KiTTY: place over the calling window (or a remembered spot), DPI/multi-
         * monitor-safe, instead of the template's screen-centre default. */
        kitty_auxpos_apply(hwnd, "About", GetWindow(hwnd, GW_OWNER), 0);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
          case IDA_LICENCE:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(IDD_LICENCEBOX),
                      hwnd, LicenceProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;

          case IDA_WEB:
            /* Load web browser (this fork's GitHub home / releases) */
            ShellExecute(hwnd, "open",
                         "https://github.com/hknet/KiTTY",
                         0, 0, SW_SHOWDEFAULT);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        kitty_auxpos_save(hwnd, "About");
        kitty_about_dlg = NULL;
        ShinyRemoveAuxDialog(hwnd);
        return 0;
    }
    return 0;
}

/*
 * Null dialog procedure.
 */
static INT_PTR CALLBACK NullDlgProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    return 0;
}

/*
 * The configuration box's own vertical geometry, in dialog units, kept
 * together and derived from ONE number so they cannot drift apart.
 *
 * CFGBOX_H must match the height of IDD_MAINBOX in windows/putty-common.rc2.
 * It came down from 402 when the box moved to Segoe UI 9: the same template
 * is ~15% taller in pixels in that font, which took the window past the
 * 1280x660 logical budget a 1080p laptop at 150% has. The panels that no
 * longer fit SCROLL - see kitty_cfg_panel_scrollbar - so the box no longer
 * has to be as tall as its tallest panel.
 */
#define CFGBOX_H             320   /* == IDD_MAINBOX height in the template */
#define CFGBOX_BUTTONROW_DU  (CFGBOX_H - 17)   /* top of the button row */
#define CFGBOX_TREE_DU       (CFGBOX_BUTTONROW_DU - 17)  /* tree height */
/* Width kept clear at the right of every panel for the scroll bar. */
#define CFGBOX_SCROLLGUTTER_DU 10
/*
 * The Session | Application tab strip above the category tree (design §9.2).
 * It replaces the "Category:" static rather than being added to the column,
 * so the cost to the tree is this height MINUS the static's 10 - and the
 * panel area, which is where the height budget actually hurts, pays nothing.
 */
#define CFGBOX_TABSTRIP_DU 14

enum {
    IDCX_ABOUT = IDC_ABOUT,
    /* KiTTY: the Session | Application tab strip, which stands where the
     * "Category:" static used to and therefore takes over its id rather than
     * adding one. These ids are not private - the QA harnesses find the
     * bottom-row buttons by id range - so a new member in the middle would
     * renumber every control after it. */
    IDCX_TABSTRIP,
    IDCX_TREEVIEW,
    IDCX_PANELSCROLL,      /* KiTTY: the panel area's scroll bar */
    IDCX_STDBASE,
    IDCX_PANELBASE = IDCX_STDBASE + 32
};

struct treeview_faff {
    HWND treeview;
    HTREEITEM lastat[4];
};

/* KiTTY: how many category-tree levels to auto-expand ([ConfigBox]
 * categoryexpand). Default 99 = full expansion; kitty.c overrides it from
 * kitty.ini. Defined here (guiterminal lib) so the stock variants, which do not
 * link kitty.c, still resolve it and simply keep the full-expansion default. */
int kitty_category_expand_depth = 99;

/* KiTTY: the tree's display names live in one table, so a translation can
 * change what is SHOWN while every path stays the identifier it is. */
#include "../kitty/kitty_tree_text.h"

static HTREEITEM treeview_insert(struct treeview_faff *faff,
                                 int level, char *text, char *path)
{
    TVINSERTSTRUCT ins;
    int i;
    HTREEITEM newitem;
    text = (char *)kitty_tree_label(text);
    ins.hParent = (level > 0 ? faff->lastat[level - 1] : TVI_ROOT);
    ins.hInsertAfter = faff->lastat[level];
#if _WIN32_IE >= 0x0400 && defined NONAMELESSUNION
#define INSITEM DUMMYUNIONNAME.item
#else
#define INSITEM item
#endif
    ins.INSITEM.mask = TVIF_TEXT | TVIF_PARAM;
    ins.INSITEM.pszText = text;
    ins.INSITEM.cchTextMax = strlen(text)+1;
    ins.INSITEM.lParam = (LPARAM)path;
    newitem = TreeView_InsertItem(faff->treeview, &ins);
    if (level > 0)
        /* KiTTY: expand the category tree to a configurable depth. `level` is the
         * inserted child's depth, so the node we expand (its parent) is at
         * user-level `level`; expand it while that is within categoryexpand
         * (default 99 = full). Stock PuTTY collapsed everything below level 1. */
        TreeView_Expand(faff->treeview, faff->lastat[level - 1],
                        (level <= kitty_category_expand_depth ? TVE_EXPAND : TVE_COLLAPSE));
    faff->lastat[level] = newitem;
    for (i = level + 1; i < 4; i++)
        faff->lastat[i] = NULL;
    return newitem;
}

Filename *dialog_box_demo_screenshot_filename = NULL;

/* ctrltrees indices for the main dialog box */
enum {
    TREE_PANEL, /* things we swap out every time treeview selects a new pane */
    TREE_BASE, /* fixed things at the bottom like OK and Cancel buttons */
};

/*
 * KiTTY: panel cache - create each panel's controls ONCE and show/hide them.
 *
 * Upstream destroys every control in the visible panel and creates the new
 * panel's set from scratch on every treeview switch. Profiled, nearly all of
 * the switch time was NtUserCreateWindowEx - window creation itself - so the
 * fix is to keep the windows. Panels are created lazily on first visit (so
 * opening the box pays for one panel, as before) and then only shown/hidden.
 *
 * Three invariants make the cache safe:
 *  - every panel gets its own disjoint dialog-id block (WM_COMMAND carries
 *    the id in a 16-bit word, so all blocks must stay below 0x10000);
 *  - all cached panels' winctrls live in the ONE shared TREE_PANEL tree, so
 *    lookups by id or by ctrl, and a full dlg_refresh(NULL), see every panel
 *    - a Load Session must update panels that are not currently visible;
 *  - only the VISIBLE panel's keyboard shortcuts are registered in the
 *    dlgparam table, or two panels using the same letter would collide.
 *
 * Contract kept from the rebuild design: a panel receives EVENT_REFRESH every
 * time it becomes visible, exactly as it did after every rebuild - handlers
 * rely on refresh-follows-build to fill their fields.
 */
#define KITTY_PANEL_ID_STRIDE 512

struct kitty_cfg_panel {
    char *path;                     /* treeview path, the cache key */
    int base_id;                    /* this panel's dialog-id block */
    struct winctrl **ctrls;         /* members, in creation order */
    size_t nctrls, ctrlsize;
    /*
     * KiTTY: how tall this panel's contents are, and how far it is currently
     * scrolled - both in pixels, both measured at creation time from the
     * controls themselves rather than predicted from the layout.
     *
     * A panel taller than the area it sits in scrolls; one that fits does
     * not, and its scroll bar is not merely disabled but ABSENT - a control
     * that can do nothing has no business taking up space or catching the
     * eye. The scroll offset is NOT kept here: it belongs to the host, which
     * holds every panel's controls at once - see kitty_cfg_scroll_y.
     */
    int content_h;
};

/* A panel has just been laid out: place any controls it positions itself.
 * kitty_config.c, stubbed to nothing for the stock variants. */
void kitty_config_panel_placed(const char *path);

/* Does the named-proxy panel hold an edit the store has not got? The width
 * reflow rebuilds panels by destroying their controls, so it leaves that one
 * alone while this is true. kitty_proxy_gui.c; false in the stock variants,
 * which have no such panel. */
bool kitty_proxy_panel_dirty(void);

static struct kitty_cfg_panel **kitty_cfg_panels = NULL;
static size_t kitty_cfg_npanels = 0, kitty_cfg_panelsize = 0;
static struct kitty_cfg_panel *kitty_cfg_active_panel = NULL;
/* Top of the fixed button strip, in client pixels - the floor of the panel
 * area. Set once the row exists, because it moves with cb_extra_du. */
static int kitty_cfg_buttonrow_top = 0;

/*
 * The PANEL HOST: the embedded child dialog every panel's controls live in.
 * See IDD_PANELHOST in windows/putty-common.rc2 for why it exists.
 *
 * Everything that used to reach a panel control with
 * GetDlgItem(dp->hwnd, id) has to go through kitty_cfg_item() instead, which
 * looks in both - the button row is still on the dialog itself.
 */
extern HWND kitty_cfg_panel_host;   /* defined in windows/controls.c */
HWND kitty_cfg_item(HWND dlg, int id);

/*
 * How far the HOST is scrolled - one number for the window, not one per
 * panel. Every panel's controls are children of the same host, so
 * ScrollWindow moves all of them at once: a per-panel offset meant that
 * scrolling one tall panel left the host displaced, and every panel selected
 * afterwards had its content pushed out of sight. Switching panels resets it.
 */
static int kitty_cfg_scroll_y = 0;

/* Pixels one wheel notch scrolls: the user's own "lines to scroll" setting,
 * times a text line. SPI_GETWHEELSCROLLLINES returns WHEEL_PAGESCROLL when
 * they have asked for a page at a time. */
static int kitty_cfg_wheel_lines(void)
{
    UINT lines = 3;
    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL)
        return 240;
    if (lines < 1)
        lines = 1;
    return (int)lines * 16;
}

/*
 * The category tree's wheel, one item at a time.
 *
 * Left to itself the tree jumps three rows per notch and ignores anything
 * smaller, so a trackpad's stream of tiny deltas moves it not at all and then
 * all at once. Same cure as the panel: keep the remainder, and move in single
 * rows - a row is the finest step a tree view has, its scroll bar counting
 * items rather than pixels.
 */
static void kitty_cfg_tree_wheel(HWND tv, WPARAM wParam)
{
    static int remainder = 0;
    UINT lines = 3;
    int items, i;

    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL)
        lines = (UINT)TreeView_GetVisibleCount(tv);
    if (lines < 1)
        lines = 1;

    remainder += GET_WHEEL_DELTA_WPARAM(wParam) * (int)lines;
    items = remainder / WHEEL_DELTA;
    remainder -= items * WHEEL_DELTA;

    for (i = 0; i < (items < 0 ? -items : items); i++)
        SendMessage(tv, WM_VSCROLL,
                    MAKEWPARAM(items > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
}

static WNDPROC kitty_cfg_tree_oldproc = NULL;

static LRESULT CALLBACK KittyCfgTreeProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    /* Only when the tree has the focus does Windows deliver the wheel here;
     * pointing at it without clicking goes to the box, which forwards. Both
     * roads have to end in the same place or the two feel different. */
    if (msg == WM_MOUSEWHEEL) {
        kitty_cfg_tree_wheel(hwnd, wParam);
        return 0;
    }
    /* Ctrl+Home / Ctrl+End work like plain Home / End (which stay as they
     * are): the tree control ignores the Ctrl-modified pair, and fingers
     * trained on editors expect both spellings to reach the ends. Forwarded
     * as the unmodified key rather than reimplemented, so the two can never
     * behave differently. */
    if (msg == WM_KEYDOWN && (wParam == VK_HOME || wParam == VK_END) &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        LRESULT r;
        BYTE ks[256];
        GetKeyboardState(ks);
        ks[VK_CONTROL] &= 0x7f;        /* deliver it Ctrl-less */
        SetKeyboardState(ks);
        r = CallWindowProc(kitty_cfg_tree_oldproc, hwnd, msg, wParam, lParam);
        ks[VK_CONTROL] |= 0x80;
        SetKeyboardState(ks);
        return r;
    }
    return CallWindowProc(kitty_cfg_tree_oldproc, hwnd, msg, wParam, lParam);
}

/*
 * The host's own procedure. It owns no controls of its own: its whole job is
 * to be a window that clips and scrolls, and to hand everything its children
 * say to the configuration box, which is where all the handlers are.
 */
static INT_PTR CALLBACK PanelHostProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG:
        return 1;
      case WM_COMMAND:
      case WM_NOTIFY:
      case WM_DRAWITEM:
      case WM_MEASUREITEM:
      case WM_HSCROLL: {
        /*
         * Straight up to the configuration box, unchanged. The handlers there
         * identify controls by id, and the ids are unique across both
         * windows, so nothing has to be rewritten to understand this.
         *
         * The ANSWER has to come back down, and that is not what returning it
         * from a dialog procedure does: a DialogProc's return value means
         * "handled", and the value the sender sees is whatever DWLP_MSGRESULT
         * holds - zero unless it is set. Custom draw is the case that showed
         * it. The theme answers NM_CUSTOMDRAW for a radio button with
         * CDRF_SKIPDEFAULT, which arrived here as zero (CDRF_DODEFAULT), so
         * the control drew its label a second time over the one the theme had
         * just drawn: unreadable in dark mode, and invisible in light mode
         * because nothing custom-draws there.
         */
        LRESULT r = SendMessage(GetParent(hwnd), msg, wParam, lParam);
        SetWindowLongPtr(hwnd, DWLP_MSGRESULT, (LONG_PTR)r);
        return TRUE;
      }
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLORBTN:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORDLG: {
        /* Colours come from the same place as the rest of the box, or a dark
         * panel would sit inside a dark dialog with light controls. */
        LRESULT r = SendMessage(GetParent(hwnd), msg, wParam, lParam);
        if (r) {
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, (LONG_PTR)r);
            return (INT_PTR)r;
        }
        return 0;
      }
    }
    return 0;
}

static struct kitty_cfg_panel *kitty_cfg_panel_find(const char *path)
{
    for (size_t i = 0; i < kitty_cfg_npanels; i++)
        if (!strcmp(kitty_cfg_panels[i]->path, path))
            return kitty_cfg_panels[i];
    return NULL;
}

/*
 * Show or hide a cached panel's WINDOWS. Nothing else - in particular not the
 * shortcuts, which is why this is separate from kitty_cfg_panel_show below.
 *
 * Laying a panel out REGISTERS its shortcuts as a side effect, so a freshly
 * rebuilt panel already holds its letters and must be made visible without
 * claiming them a second time. Doing that through panel_show tripped
 * winctrl_add_shortcuts' assert (`!dp->shortcuts[s]`, controls.c) and took
 * the box down with it.
 */
static void kitty_cfg_panel_windows(struct dlgparam *dp,
                                    struct kitty_cfg_panel *p, bool show)
{
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = kitty_cfg_item(dp->hwnd, c->base_id + k);
            if (item) {
                /* SetWindowPos(..., SWP_NOREDRAW), not ShowWindow: hiding or
                 * showing a child invalidates the parent, and it was that
                 * flicker the caller used to suppress by switching the whole
                 * DIALOG's redraw off - which cost the box its WS_VISIBLE
                 * style and, with it, every click that landed during the
                 * switch (hknet/KiTTY#38). Painting is deferred instead, and
                 * the caller repaints the panel area once at the end. */
                SetWindowPos(item, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE | SWP_NOREDRAW |
                             (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
            }
        }
    }
}

/* Show or hide a cached panel's windows, and swap its shortcuts in or out of
 * the dlgparam table with it. The pairing every panel SWITCH wants. */
static void kitty_cfg_panel_show(struct dlgparam *dp,
                                 struct kitty_cfg_panel *p, bool show)
{
    kitty_cfg_panel_windows(dp, p, show);
    for (size_t i = 0; i < p->nctrls; i++) {
        if (show)
            winctrl_add_shortcuts(dp, p->ctrls[i]);
        else
            winctrl_rem_shortcuts(dp, p->ctrls[i]);
    }
}

/*
 * ------------------------------------------------------------ panel scrolling
 *
 * The panels live in the PANEL HOST (IDD_PANELHOST), a child dialog covering
 * the panel area. That is what makes this short: Windows clips the controls
 * to the host, so nothing can be drawn into the button strip below it, and
 * scrolling is ScrollWindow on the host - the OS moves the children, blits
 * what it can and invalidates the rest.
 *
 * The first attempt did all of that by hand on controls parented to the
 * dialog: moving each one, cutting the straddlers with window regions and
 * placing the repaints. It produced four separate faults - controls drawn
 * over the buttons, rows vanishing while still half visible, ghosted
 * duplicates of the previous panel, and stale pixels that the control
 * rectangles said were not there. None of them can happen here, because none
 * of that code exists any more.
 */

/* The panel area, in the DIALOG's client coordinates: where the host sits.
 * Its top clears the tab-strip band, which the session-name label shares -
 * the panels moved DOWN when that label arrived (the template grew by the
 * same amount, so no panel lost any height). */
static void kitty_cfg_panel_rect(HWND hwnd, RECT *out)
{
    RECT dlu = { 100, 3 + CFGBOX_TABSTRIP_DU, 3, 0 };
    RECT client;

    MapDialogRect(hwnd, &dlu);
    GetClientRect(hwnd, &client);
    out->left = dlu.left;
    out->top = dlu.top;
    out->right = client.right - dlu.right;
    out->bottom = client.bottom;
    /*
     * The button row is the floor, taken from the layout constant and NOT by
     * looking a button up: this box builds its buttons with generated ids and
     * has no control 1, so GetDlgItem(hwnd, IDOK) returns NULL and the floor
     * would silently fall to the bottom of the window. You shall not pass.
     */
    if (kitty_cfg_buttonrow_top > out->top)
        out->bottom = kitty_cfg_buttonrow_top - 4;
}

/* How tall a panel's contents are, in host coordinates: the lowest edge of
 * any of its controls. Measured from the controls themselves, once, while the
 * host is unscrolled. */
static int kitty_cfg_panel_measure(struct kitty_cfg_panel *p)
{
    int bottom = 0;

    if (!kitty_cfg_panel_host)
        return 0;
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(kitty_cfg_panel_host, c->base_id + k);
            RECT r;
            if (!item || !GetWindowRect(item, &r))
                continue;
            MapWindowPoints(NULL, kitty_cfg_panel_host, (POINT *)&r, 2);
            if (r.bottom > bottom)
                bottom = r.bottom;
        }
    }
    return bottom ? bottom + 4 : 0;   /* a little air at the foot */
}

/* Scroll the host to a position, clamped to what there is to see. */
static void kitty_cfg_panel_scroll_to(HWND hwnd, struct kitty_cfg_panel *p,
                                      int newy)
{
    RECT area;
    int maxy, delta;

    if (!kitty_cfg_panel_host)
        return;
    kitty_cfg_panel_rect(hwnd, &area);
    maxy = p->content_h - (area.bottom - area.top);
    if (maxy < 0)
        maxy = 0;
    if (newy < 0)
        newy = 0;
    if (newy > maxy)
        newy = maxy;
    delta = newy - kitty_cfg_scroll_y;
    if (!delta)
        return;
    kitty_cfg_scroll_y = newy;
    /*
     * BLIT, do not repaint everything.
     *
     * Repainting the whole host on every notch is correct and flickers
     * horribly - a panel's worth of drawing per wheel click. ScrollWindowEx
     * moves the children and copies the pixels that are still good, leaving
     * only the newly uncovered strip to draw.
     *
     * That blit ghosted while the host had WS_CLIPCHILDREN, because the strip
     * could not be erased under the controls. Without that style it is both
     * correct and cheap, which is why the two changes belong together.
     */
    ScrollWindowEx(kitty_cfg_panel_host, 0, -delta, NULL, NULL, NULL, NULL,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    UpdateWindow(kitty_cfg_panel_host);
    SetScrollPos(GetDlgItem(hwnd, IDCX_PANELSCROLL), SB_CTL, newy, TRUE);
}

/*
 * Fit the scroll bar to the panel now showing: shown when the panel is taller
 * than the host, and ABSENT - not merely disabled - when it is not.
 *
 * `keeppos` is for the one caller that is not a panel SWITCH: resizing the box
 * re-fits the same panel's bar, and sending that panel back to the top on
 * every WM_SIZE would yank it out from under the reader mid-drag. It keeps the
 * offset instead, clamped to what is left to see now the area has changed
 * size.
 */
static void kitty_cfg_panel_scrollbar(HWND hwnd, struct kitty_cfg_panel *p,
                                      bool keeppos)
{
    HWND sb = GetDlgItem(hwnd, IDCX_PANELSCROLL);
    RECT area;
    int areah, width;
    SCROLLINFO si;

    if (!sb)
        return;
    if (!p) {
        ShowWindow(sb, SW_HIDE);
        return;
    }
    kitty_cfg_panel_rect(hwnd, &area);
    areah = area.bottom - area.top;
    /* Whatever is showing starts at the top: the host carries the offset, so
     * a new panel would otherwise inherit the last one's. */
    if (kitty_cfg_scroll_y && !keeppos)
        kitty_cfg_panel_scroll_to(hwnd, p, 0);
    else if (kitty_cfg_scroll_y)
        kitty_cfg_panel_scroll_to(hwnd, p, kitty_cfg_scroll_y);
    if (p->content_h <= areah) {
        ShowWindow(sb, SW_HIDE);
        return;
    }

    width = GetSystemMetrics(SM_CXVSCROLL);
    /* In the strip the host leaves clear, and ABOVE it in the z-order so a
     * click can never be swallowed. */
    SetWindowPos(sb, HWND_TOP, area.right - width, area.top,
                 width, areah, SWP_NOACTIVATE);

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = p->content_h - 1;
    si.nPage = areah;
    si.nPos = kitty_cfg_scroll_y;
    SetScrollInfo(sb, SB_CTL, &si, TRUE);
    ShowWindow(sb, SW_SHOW);
}

/*
 * KiTTY: the resizable configuration box (design/TASK_ui_modernisation.md §3
 * and §11, stage A - the OUTER furniture).
 *
 * The template has no relayout pass at all, so this adds the one thing that
 * was missing: a baseline captured once, at the size the template produced,
 * and a WM_SIZE that re-places the outer furniture against it. Everything
 * that used to grow the window by a hand-computed fudge at creation time
 * (cb_extra_du) now goes through the same road - the box is created at the
 * template size, and the size that was configured is applied afterwards as a
 * resize. One layout path instead of two that could disagree.
 *
 * What is anchored: the category tree (rides the bottom edge, so it gets
 * taller), the panel host (all four edges - the scroll-bar gutter at its
 * right is a constant, and anchoring both sides preserves it), and every
 * control in the button row (rides the bottom edge). The tab strip sits at
 * the top left and does not move.
 *
 * What is NOT anchored: the panel scroll bar, which is placed from the panel
 * area every time it is fitted and so needs no baseline of its own; and the
 * panel CONTENTS, which are not moved but REBUILT - a width change at the end
 * of a drag re-runs every cached panel's layout (kitty_cfg_panel_relayout_ex),
 * hidden panels included, refusing only a panel holding unsaved edits (the
 * named-proxy editor is the one that has any).
 */
static bool kitty_cfg_layout_ready = false;
static struct kl_anchor_win *kitty_cfg_anchors = NULL;
static RECT *kitty_cfg_anchor_rects = NULL;
static size_t kitty_cfg_nanchors = 0;
static size_t kitty_cfg_anchorsize = 0;
static SIZE kitty_cfg_basesize, kitty_cfg_minsize;
/* The loaded session's name, on the tab strip's row over the panel area;
 * created with the strip, text kept current by
 * kitty_cfg_session_label_update below. */
static HWND kitty_cfg_session_label = NULL;
/* The box's size when the current drag began. What makes a move-only drag
 * distinguishable from a resize - see kitty_cfgbox_save_size. */
static SIZE kitty_cfg_dragsize;
/* The button row's top at baseline; the live value is this plus the growth,
 * and kitty_cfg_panel_rect reads the live one. */
static int kitty_cfg_buttonrow_top_base = 0;

static void kitty_cfg_anchor_add(HWND w, unsigned anchor)
{
    if (!w)
        return;
    sgrowarray(kitty_cfg_anchors, kitty_cfg_anchorsize, kitty_cfg_nanchors);
    kitty_cfg_anchors[kitty_cfg_nanchors].hwnd = w;
    kitty_cfg_anchors[kitty_cfg_nanchors].anchor = anchor;
    kitty_cfg_nanchors++;
}

static void kitty_cfg_layout_free(void)
{
    sfree(kitty_cfg_anchors);
    sfree(kitty_cfg_anchor_rects);
    kitty_cfg_anchors = NULL;
    kitty_cfg_anchor_rects = NULL;
    kitty_cfg_nanchors = kitty_cfg_anchorsize = 0;
    kitty_cfg_layout_ready = false;
}

/*
 * Called at the end of the creation phase, while the box is still exactly the
 * size the template asked for: that size becomes both the baseline the
 * relayout measures growth from and the minimum the box may be dragged to.
 */
static void kitty_cfg_layout_capture(PortableDialogStuff *pds, HWND hwnd)
{
    struct winctrl *c;

    kitty_cfg_layout_free();

    kitty_cfg_anchor_add(GetDlgItem(hwnd, IDCX_TREEVIEW),
                         KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_BOTTOM);
    kitty_cfg_anchor_add(kitty_cfg_panel_host,
                         KL_ANCH_LEFT | KL_ANCH_TOP |
                         KL_ANCH_RIGHT | KL_ANCH_BOTTOM);
    /* The session-name label spans the panel area's width on the strip's
     * row, so it keeps centring as the box is widened. */
    kitty_cfg_anchor_add(kitty_cfg_session_label,
                         KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT);
    /*
     * The button row, walked rather than tabulated: this box hands its
     * controls generated ids (it has no control 1 - see kitty_cfg_panel_rect),
     * so there is no id table to anchor against and GetDlgItem(hwnd, IDOK)
     * finds nothing. The winctrls tree is where the row's real ids are.
     */
    for (int i = 0;
         (c = winctrl_findbyindex(&pds->ctrltrees[TREE_BASE], i)) != NULL; i++)
        for (int k = 0; k < c->num_ids; k++)
            kitty_cfg_anchor_add(GetDlgItem(hwnd, c->base_id + k),
                                 KL_ANCH_LEFT | KL_ANCH_BOTTOM);

    kitty_cfg_anchor_rects = snewn(kitty_cfg_nanchors, RECT);
    anchored_capture_windows(hwnd, kitty_cfg_anchors, kitty_cfg_nanchors,
                             kitty_cfg_anchor_rects, &kitty_cfg_basesize,
                             &kitty_cfg_minsize);
    kitty_cfg_buttonrow_top_base = kitty_cfg_buttonrow_top;
    kitty_cfg_dragsize = kitty_cfg_minsize;   /* no drag in progress */
    kitty_cfg_layout_ready = true;
}

static void kitty_cfg_layout_relayout(HWND hwnd)
{
    RECT rc;

    if (!kitty_cfg_layout_ready)
        return;                 /* WM_SIZE can arrive before there is a layout */
    GetClientRect(hwnd, &rc);
    anchored_relayout_windows(hwnd, kitty_cfg_anchors, kitty_cfg_nanchors,
                              kitty_cfg_anchor_rects, kitty_cfg_basesize);
    /* The floor the panel area and the scrolling both measure against moved
     * with the button row, and has to be told so. */
    kitty_cfg_buttonrow_top = kitty_cfg_buttonrow_top_base +
        ((rc.bottom - rc.top) - kitty_cfg_basesize.cy);
    /* The visible panel now has more (or less) area to be seen in. Keep where
     * the reader had scrolled to; the bar clamps it. */
    if (kitty_cfg_active_panel)
        kitty_cfg_panel_scrollbar(hwnd, kitty_cfg_active_panel, true);
    /* Everything pinned to the panel area's bottom edge - the Proxy panel's
     * pre-set loader and the application panels' footers - follows the edge,
     * which just moved. (Stubbed to nothing in the stock variants.) */
    {
        extern void kitty_config_pin_bottoms(void);
        kitty_config_pin_bottoms();
    }
}

/* EVENT_REFRESH for one cached panel, in creation order - the scoped
 * equivalent of what dlg_refresh(NULL) did when only one panel existed. */
static void kitty_cfg_panel_refresh(struct dlgparam *dp,
                                    struct kitty_cfg_panel *p)
{
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        if (c->ctrl && c->ctrl->handler != NULL)
            c->ctrl->handler(c->ctrl, dp, dp->data, EVENT_REFRESH);
    }
}

/* EVENT_REFRESH for one whole winctrls tree (the fixed button row). */
static void kitty_cfg_winctrls_refresh(struct dlgparam *dp,
                                       struct winctrls *wc)
{
    struct winctrl *c;
    for (int i = 0; (c = winctrl_findbyindex(wc, i)) != NULL; i++)
        if (c->ctrl && c->ctrl->handler != NULL)
            c->ctrl->handler(c->ctrl, dp, dp->data, EVENT_REFRESH);
}

/*
 * Move one panel's windows by dy, without touching anything else in the host.
 *
 * A panel is always LAID OUT at its unscrolled position, because ctlposinit
 * works in the host's client coordinates and knows nothing about scrolling.
 * That is fine when the host is at the top, and wrong the moment it is not:
 * the panel being created lands dy pixels below every other child, and the
 * next reset - which scrolls the whole host back by a blit - carries it that
 * far past the top, leaving a blank band exactly as tall as the offset was.
 * A panel can be created while scrolled by either road: switching to one that
 * has never been shown, or the background warm-up building the rest.
 *
 * So the new panel is brought into line with the host as soon as it exists.
 * Its windows are still hidden at that point, so nothing flickers.
 */
static void kitty_cfg_panel_offset(struct kitty_cfg_panel *p, int dy)
{
    if (!dy || !kitty_cfg_panel_host)
        return;
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int j = 0; j < c->num_ids; j++) {
            HWND h = GetDlgItem(kitty_cfg_panel_host, c->base_id + j);
            RECT r;
            POINT pt;
            if (!h)
                continue;
            GetWindowRect(h, &r);
            pt.x = r.left; pt.y = r.top;
            ScreenToClient(kitty_cfg_panel_host, &pt);
            SetWindowPos(h, NULL, pt.x, pt.y + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

/* Create a panel's controls (visible, shortcuts registered - creation IS
 * showing) and record it in the cache. The controls are laid out into a
 * scratch tree, then moved one by one into the shared TREE_PANEL tree; the
 * drain preserves creation order because winctrl_findbyindex walks the byid
 * tree and ids ascend as they are handed out. */
/* Lay a panel's controls out into the host. `p` arrives with its path and its
 * id block already decided and no controls; it leaves built. Split out of
 * kitty_cfg_panel_create so that a panel can be laid out a SECOND time,
 * into the same id block, without becoming a new entry in the cache. */
static void kitty_cfg_panel_build(PortableDialogStuff *pds,
                                  struct kitty_cfg_panel *p)
{
    struct winctrls scratch;
    winctrl_init(&scratch);

    struct ctlpos cp;
    /*
     * The right border reserves room for the panel scroll bar - ALWAYS, not
     * only on the panels that need one. The bar appearing would otherwise
     * overlap the right-hand controls, and making the panel narrower at that
     * moment would mean re-laying it out and having every control jump
     * sideways the first time a panel is scrolled. A few units of white space
     * on the panels that do not scroll is the cheaper of the two.
     */
    ctlposinit(&cp, kitty_cfg_panel_host, 0, CFGBOX_SCROLLGUTTER_DU, 0);
    /*
     * Tell doctl's push-button rule how wide this panel WOULD be at the
     * template size: a push button is held at that width unless it spans the
     * full content width (only those stretch). Zero - rule inactive - while
     * the box is at the template size or the baseline is not captured yet,
     * so the template-size layout is exactly what it always was.
     */
    {
        extern int kitty_cfg_btn_basew_du, kitty_cfg_btn_fullw_du;
        RECT dlgrc;
        kitty_cfg_btn_fullw_du = cp.width;
        kitty_cfg_btn_basew_du = 0;
        if (kitty_cfg_layout_ready && cp.dlu4inpix > 0 &&
            GetClientRect(pds->dp->hwnd, &dlgrc)) {
            int grow_px = (dlgrc.right - dlgrc.left) - kitty_cfg_basesize.cx;
            if (grow_px > 0)
                kitty_cfg_btn_basew_du =
                    cp.width - (grow_px * 4) / cp.dlu4inpix;
        }
    }
    int id = p->base_id;
    for (int index = -1; (index = ctrl_find_path(
                              pds->ctrlbox, p->path, index)) >= 0 ;) {
        struct controlset *s = pds->ctrlbox->ctrlsets[index];
        winctrl_layout(pds->dp, &scratch, &cp, s, &id);
    }
    {
        extern int kitty_cfg_btn_basew_du;
        kitty_cfg_btn_basew_du = 0;   /* other dialogs lay out untouched */
    }
    assert(id - p->base_id <= KITTY_PANEL_ID_STRIDE);

    struct winctrl *c;
    while ((c = winctrl_findbyindex(&scratch, 0)) != NULL) {
        winctrl_remove(&scratch, c);
        winctrl_add(&pds->ctrltrees[TREE_PANEL], c);
        sgrowarray(p->ctrls, p->ctrlsize, p->nctrls);
        p->ctrls[p->nctrls++] = c;
    }
    winctrl_cleanup(&scratch);

    /* These controls did not exist when the box was themed, so nothing that
     * has to be SENT to a control has reached them: without this a cached
     * panel comes up in the classic colours the first time it is shown. */
    kitty_theme_refresh(pds->dp->hwnd);
    /*
     * KiTTY: snap every label sitting beside a drop-down onto the middle of
     * it.
     *
     * This cannot be done from the layout. A combo box IGNORES the height it
     * is created with and sizes itself from the font - 23px where the 14
     * dialog units it was given come to 19 - so a label built to the same
     * nominal height as the combo beside it ends up 2px high, on every
     * drop-down row in the box. The numbers only exist once Windows has made
     * the controls, so the correction belongs here, after they exist. (The
     * same reasoning produced the row-aligner in kageant's key-list window.)
     *
     * Matched on CLASS rather than on control type: every helper that puts a
     * label beside a combo lays them out as consecutive ids, and the pair is
     * exactly "a STATIC then a COMBOBOX". That covers drop-down lists and
     * editable combos without either of them having to be enumerated here.
     */
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        HWND lbl, cmb;
        RECT lr, cr;
        char cls[32];

        if (c->num_ids < 2)
            continue;
        lbl = GetDlgItem(kitty_cfg_panel_host, c->base_id);
        cmb = GetDlgItem(kitty_cfg_panel_host, c->base_id + 1);
        if (!lbl || !cmb)
            continue;
        if (!GetClassNameA(lbl, cls, sizeof(cls)) || stricmp(cls, "STATIC"))
            continue;
        if (!GetClassNameA(cmb, cls, sizeof(cls)) || stricmp(cls, "COMBOBOX"))
            continue;
        if (!GetWindowRect(lbl, &lr) || !GetWindowRect(cmb, &cr))
            continue;
        /* Only a row: a label ABOVE its combo (the full-width form) is not
         * one, and must not be dragged down into it. */
        if (lr.top >= cr.bottom || lr.bottom <= cr.top)
            continue;
        {
            int dy = ((cr.top + cr.bottom) - (lr.top + lr.bottom)) / 2;
            POINT pt;
            if (!dy)
                continue;
            pt.x = lr.left; pt.y = lr.top + dy;
            ScreenToClient(kitty_cfg_panel_host, &pt);
            SetWindowPos(lbl, NULL, pt.x, pt.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    /*
     * KiTTY: then centre every ROW on itself, buttons included. An EDIT or
     * COMBOBOX re-sizes itself to the font once created, while a push
     * button keeps its template height, so a row mixing them comes out of
     * the layout top-aligned with the buttons hanging below the field -
     * a pixel-line at 96 DPI, plainly crooked at 200% (the Broadcast key
     * row, 2026-08-31). The label/combo snap above reaches only a STATIC
     * and COMBOBOX inside ONE control; this pass works across controls
     * sharing a row. Anything taller than 20 dialog units - list boxes,
     * multi-line texts - takes no part: a button beside a tall list is
     * placed, not centred. Nor does a group box, which FRAMES rows without
     * being on one.
     */
    {
        struct { HWND h; RECT r; size_t row; } *rows = NULL;
        size_t nrows = 0, rowsize = 0;
        RECT du = { 0, 0, 4, 20 };
        MapDialogRect(kitty_cfg_panel_host, &du); /* du.bottom: 20 DLU in px */
        for (size_t i = 0; i < p->nctrls; i++) {
            struct winctrl *c = p->ctrls[i];
            for (int k = 0; k < c->num_ids; k++) {
                HWND h = GetDlgItem(kitty_cfg_panel_host, c->base_id + k);
                RECT r;
                char cls[32];
                if (!h || !GetWindowRect(h, &r))
                    continue;
                if (r.bottom - r.top <= 0 || r.bottom - r.top > du.bottom)
                    continue;
                if (GetClassNameA(h, cls, sizeof(cls)) &&
                    !stricmp(cls, "Button") &&
                    (GetWindowLongPtr(h, GWL_STYLE) & 0xF) == BS_GROUPBOX)
                    continue;
                sgrowarray(rows, rowsize, nrows);
                rows[nrows].h = h;
                rows[nrows].r = r;
                rows[nrows].row = nrows;
                nrows++;
            }
        }
        /* Same row = vertical spans overlapping by more than half the
         * shorter control. Labels propagate until stable, so a row is the
         * transitive closure and a chain of overlaps stays one row. */
        for (bool changed = true; changed; ) {
            changed = false;
            for (size_t i = 0; i < nrows; i++) {
                for (size_t j = i + 1; j < nrows; j++) {
                    int ov = (int)(min(rows[i].r.bottom, rows[j].r.bottom) -
                                   max(rows[i].r.top, rows[j].r.top));
                    int hi = (int)(rows[i].r.bottom - rows[i].r.top);
                    int hj = (int)(rows[j].r.bottom - rows[j].r.top);
                    if (ov * 2 > (hi < hj ? hi : hj) &&
                        rows[i].row != rows[j].row) {
                        size_t m = rows[i].row < rows[j].row ?
                                   rows[i].row : rows[j].row;
                        rows[i].row = rows[j].row = m;
                        changed = true;
                    }
                }
            }
        }
        for (size_t i = 0; i < nrows; i++) {
            LONG top = rows[i].r.top, bottom = rows[i].r.bottom;
            for (size_t j = 0; j < nrows; j++) {
                if (rows[j].row != rows[i].row)
                    continue;
                if (rows[j].r.top < top) top = rows[j].r.top;
                if (rows[j].r.bottom > bottom) bottom = rows[j].r.bottom;
            }
            {
                int h = (int)(rows[i].r.bottom - rows[i].r.top);
                int dy = (int)((top + bottom) / 2 -
                               (rows[i].r.top + rows[i].r.bottom) / 2);
                POINT pt;
                if (!dy)
                    continue;
                pt.x = rows[i].r.left; pt.y = rows[i].r.top + dy;
                ScreenToClient(kitty_cfg_panel_host, &pt);
                SetWindowPos(rows[i].h, NULL, pt.x, pt.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                (void)h;
            }
        }
        sfree(rows);
    }
    /*
     * KiTTY: panels that place some of their own controls get to do it here,
     * after the generic layout and BEFORE the panel is measured - the buttons
     * beside the saved-session list are moved, so where they end up decides
     * how tall this panel is. Stubbed to nothing in the stock variants.
     */
    kitty_config_panel_placed(p->path);
    /* Measured now, while the controls are at their unscrolled positions. */
    p->content_h = kitty_cfg_panel_measure(p);
    /* ... and only then brought into line with a host that is scrolled. */
    kitty_cfg_panel_offset(p, -kitty_cfg_scroll_y);
}

static struct kitty_cfg_panel *kitty_cfg_panel_create(
    PortableDialogStuff *pds, const char *path)
{
    struct kitty_cfg_panel *p = snew(struct kitty_cfg_panel);
    p->path = dupstr(path);
    p->base_id = IDCX_PANELBASE +
        (int)kitty_cfg_npanels * KITTY_PANEL_ID_STRIDE;
    p->ctrls = NULL;
    p->nctrls = p->ctrlsize = 0;
    sgrowarray(kitty_cfg_panels, kitty_cfg_panelsize, kitty_cfg_npanels);
    kitty_cfg_panels[kitty_cfg_npanels++] = p;
    kitty_cfg_panel_build(pds, p);
    return p;
}

/*
 * Throw one cached panel away, windows and all, so the next visit builds it
 * again from the current settings.
 *
 * This is the primitive a LAYOUT change needs, as opposed to a value change:
 * a panel's geometry is fixed when winctrl_layout runs, so a setting that
 * decides how big a control is - the saved-session list's row count is the
 * one that does - cannot be applied by refreshing the panel. It has to be
 * laid out again.
 *
 * ⛔ NOT for the panel the user is looking at, and NOT for a panel holding
 * edits that have not been stored. Rebuilding destroys the windows, so
 * anything typed into them and not yet written is gone. Every Application
 * panel writes as it is touched, and the session panels are backed by conf,
 * so the one that matters is the named-proxy editor, which edits records and
 * keeps a dirty flag. The caller is refused if either applies rather than
 * being trusted to remember.
 */
static void kitty_cfg_panel_shortcuts(struct dlgparam *dp,
                                      struct kitty_cfg_panel *p, bool add);

/*
 * Lay one cached panel out again.
 *
 * `visible` says the panel is the one on screen, which the caller must know
 * because the two cases differ in what has to be put back afterwards: a
 * hidden panel is rebuilt hidden and that is all, while the one on screen
 * has to be shown again, refreshed so its controls carry values rather than
 * the blanks they are created with, and have its focus and scroll position
 * restored. Rebuilding the visible panel is only for a WIDTH change, where
 * leaving it alone means leaving it clipped.
 */
static bool kitty_cfg_panel_relayout_ex(PortableDialogStuff *pds,
                                        struct kitty_cfg_panel *p,
                                        bool visible)
{
    int focus_index = -1;
    int scroll_was = kitty_cfg_scroll_y;

    if (!p || !kitty_cfg_panel_host || !pds || !pds->dp)
        return false;
    if ((p == kitty_cfg_active_panel) != visible)
        return false;                  /* the caller has them the wrong way round */
    /*
     * The named-proxy editor is the one panel holding something the store has
     * not got. Destroying its controls would throw the edit away without
     * asking, so it keeps the width it has until it is saved or left - a
     * clipped panel is recoverable, a discarded edit is not.
     */
    if (kitty_proxy_panel_dirty() && !strcmp(p->path, "Application/Named proxies"))
        return false;

    if (visible) {
        /* Which control had the focus, as an INDEX into this panel - the
         * windows themselves are about to stop existing. */
        HWND focus = GetFocus();
        for (size_t i = 0; i < p->nctrls && focus; i++) {
            struct winctrl *c = p->ctrls[i];
            for (int k = 0; k < c->num_ids; k++)
                if (kitty_cfg_item(pds->dp->hwnd, c->base_id + k) == focus) {
                    focus_index = (int)i;
                    break;
                }
            if (focus_index >= 0)
                break;
        }
        /* Its shortcuts ARE registered - it is showing - so withdraw them
         * before the rebuild registers them again. */
        kitty_cfg_panel_shortcuts(pds->dp, p, false);
        /* Back to the top while the controls are replaced: the host carries
         * the scroll offset and the new controls are laid out unscrolled. */
        if (kitty_cfg_scroll_y)
            kitty_cfg_panel_scroll_to(pds->dp->hwnd, p, 0);
    }

    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = kitty_cfg_item(pds->dp->hwnd, c->base_id + k);
            if (item)
                DestroyWindow(item);
        }
        /*
         * The shortcuts left with kitty_cfg_panel_show(false) when this panel
         * was hidden, so there are none to withdraw here - but the winctrl
         * belongs to the SHARED tree and has to leave it, or pds_free later
         * frees a structure whose windows are long gone.
         */
        winctrl_remove(&pds->ctrltrees[TREE_PANEL], c);
        sfree(c->data);
        sfree(c);
    }
    p->nctrls = 0;                  /* the array itself is reused */

    /*
     * The SAME cache slot and the SAME id block.
     *
     * base_id is derived from the panel's index in the cache, so the entry
     * must not move and must not be removed: taking it out and letting the
     * next create hand out an id block by index would give the rebuilt panel
     * a block another panel is already using. Rebuilding in place keeps every
     * id exactly where it was, which also means the harnesses that find these
     * controls by id are unaffected.
     */
    {
        /*
         * The same two precautions the background warm-up takes, and for the
         * same reasons: controls are created HIDDEN, because one that exists
         * visible for even a single dispatch re-points the mouse cursor if
         * the pointer is over it; and the visible panel's keyboard shortcuts
         * are parked for the duration, because laying a panel out registers
         * its shortcuts and the same letter claimed twice trips
         * winctrl_add_shortcuts' assert - which is a crash, not a glitch.
         */
        extern bool kitty_cfg_create_hidden;
        if (!visible && kitty_cfg_active_panel)
            kitty_cfg_panel_shortcuts(pds->dp, kitty_cfg_active_panel, false);
        kitty_cfg_create_hidden = true;
        kitty_cfg_panel_build(pds, p);
        kitty_cfg_create_hidden = false;
        if (!visible) {
            kitty_cfg_panel_show(pds->dp, p, false);
            if (kitty_cfg_active_panel)
                kitty_cfg_panel_shortcuts(pds->dp, kitty_cfg_active_panel, true);
        }
    }

    if (visible) {
        /*
         * Put back everything the rebuild took away: show the windows, then
         * fill them - a control is created EMPTY, and only EVENT_REFRESH puts
         * the setting into it. Skipping that would leave the reader looking
         * at a panel of blanks.
         *
         * WINDOWS ONLY. Laying the panel out has already registered its
         * shortcuts; going through panel_show would claim the same letters a
         * second time and trip winctrl_add_shortcuts' assert.
         */
        kitty_cfg_panel_windows(pds->dp, p, true);
        kitty_cfg_panel_refresh(pds->dp, p);
        kitty_cfg_panel_scrollbar(pds->dp->hwnd, p, false);
        if (scroll_was)
            kitty_cfg_panel_scroll_to(pds->dp->hwnd, p, scroll_was);
        if (focus_index >= 0 && (size_t)focus_index < p->nctrls) {
            HWND h = kitty_cfg_item(pds->dp->hwnd,
                                    p->ctrls[focus_index]->base_id);
            if (h)
                SetFocus(h);
        }
        InvalidateRect(kitty_cfg_panel_host, NULL, TRUE);
    }
    return true;
}

static bool kitty_cfg_panel_relayout(PortableDialogStuff *pds,
                                     const char *path)
{
    return kitty_cfg_panel_relayout_ex(pds, kitty_cfg_panel_find(path), false);
}

/* Shortcut registration alone, without touching window visibility - the
 * warm-up below needs the two separated. */
static void kitty_cfg_panel_shortcuts(struct dlgparam *dp,
                                      struct kitty_cfg_panel *p, bool add)
{
    for (size_t i = 0; i < p->nctrls; i++) {
        if (add)
            winctrl_add_shortcuts(dp, p->ctrls[i]);
        else
            winctrl_rem_shortcuts(dp, p->ctrls[i]);
    }
}

/*
 * Background warm-up: build one not-yet-cached panel, hidden, per call.
 * Driven by a timer armed after the box opens, so that by the time the user
 * clicks a category its panel already exists and the switch costs a
 * show/hide, not a build - first visits become as fast as revisits.
 *
 * Two things make this safe:
 *  - the whole step runs inside one message dispatch with the dialog's
 *    redraw OFF. The new controls are created and hidden before any WM_PAINT
 *    can run, and invalidations queued against a redraw-off window are
 *    DISCARDED, so the visible panel's pixels are never touched and nothing
 *    repaints when redraw comes back on;
 *  - creation registers the new panel's keyboard shortcuts as a side effect
 *    of layout, and the VISIBLE panel is holding its own letters - the same
 *    letter on both would trip winctrl_add_shortcuts' collision assert. So
 *    the visible panel's shortcuts are parked for the duration of the step
 *    and restored afterwards; no user input can arrive mid-dispatch.
 *
 * The warm panel gets NO refresh here: the show-time EVENT_REFRESH contract
 * (see the cache comment above) supplies its values on first visit, exactly
 * as it does for a revisit.
 *
 * Returns false when every panel is cached and the timer can stop.
 */
static bool kitty_cfg_warmup_step(PortableDialogStuff *pds)
{
    char *path = NULL;
    for (int i = 0; i < pds->ctrlbox->nctrlsets; i++) {
        struct controlset *s = pds->ctrlbox->ctrlsets[i];
        if (!s->pathname[0])
            continue;
        if (kitty_cfg_panel_find(s->pathname))
            continue;              /* also skips same-path siblings: once the
                                    * first ctrlset's panel is built, find()
                                    * answers for the rest of its path */
        path = s->pathname;
        break;
    }
    if (!path)
        return false;

    /*
     * NO WM_SETREDRAW HERE, and it must stay that way: sent to a TOP-LEVEL
     * window, WM_SETREDRAW(FALSE) strips WS_VISIBLE from it. The box keeps
     * its pixels but drops out of hit-testing, so for those milliseconds a
     * click goes straight THROUGH it to whatever is behind - which then
     * takes the foreground and the box appears to fall to the background by
     * itself. This ran on a 120 ms timer for the whole warm-up, leaving the
     * box unclickable for about 5% of the first seconds: measured on
     * 2026-08-25 by polling the window style from another process, 32,105
     * of 602,604 samples, in OFF/ON pairs 10-40 ms apart. Small, and it
     * still swallowed the click that made hknet/KiTTY#38 - a fifth of the
     * clicks in that window is not the same as a fifth of the time.
     *
     * It is not needed either: the panel is built with its controls hidden
     * (kitty_cfg_create_hidden), so this step paints nothing to suppress.
     */
    if (kitty_cfg_active_panel)
        kitty_cfg_panel_shortcuts(pds->dp, kitty_cfg_active_panel, false);
    {
        /* Created hidden (see kitty_cfg_create_hidden in controls.c): a
         * control that exists visible for even one dispatch re-points the
         * mouse cursor if the pointer is over it. */
        extern bool kitty_cfg_create_hidden;
        struct kitty_cfg_panel *p;
        kitty_cfg_create_hidden = true;
        p = kitty_cfg_panel_create(pds, path);
        kitty_cfg_create_hidden = false;
        kitty_cfg_panel_show(pds->dp, p, false);
    }
    if (kitty_cfg_active_panel)
        kitty_cfg_panel_shortcuts(pds->dp, kitty_cfg_active_panel, true);
    return true;
}

/* The winctrl structures themselves belong to the TREE_PANEL tree and are
 * freed with it in pds_free; this frees only the cache's own bookkeeping.
 * Runs at both open and close of the box, so a cache can never leak from one
 * instance into the next (the mid-session box builds a DIFFERENT panel set). */
static void kitty_cfg_panel_cache_reset(void)
{
    for (size_t i = 0; i < kitty_cfg_npanels; i++) {
        sfree(kitty_cfg_panels[i]->path);
        sfree(kitty_cfg_panels[i]->ctrls);
        sfree(kitty_cfg_panels[i]);
    }
    sfree(kitty_cfg_panels);
    kitty_cfg_panels = NULL;
    kitty_cfg_npanels = kitty_cfg_panelsize = 0;
    kitty_cfg_active_panel = NULL;
}

/*
 * This function is the configuration box.
 * (Being a dialog procedure, in general it returns 0 if the default
 * dialog processing should be performed, and 1 if it should not.)
 */
/* KiTTY: remember the configuration dialog's own window position. Stored as a
 * top-left point under HKCU\<reg base>\WindowPos\ConfigBox; restored on open if
 * it's still on a visible monitor, otherwise the box is centred as before.
 * NOT guarded by MOD_PERSO: dialog.c compiles into the shared guiterminal lib,
 * which is built WITHOUT MOD_PERSO (kitty_registry_base is still linked in). */
extern const char *kitty_registry_base(void);   /* windows/storage.c */
/* Off by default; set KITTY_WINPOS_DEBUG=1 to trace config-box positioning to
 * %TEMP%\kitty_winpos.log (shared with the main-window winpos trace). */
static int kitty_cfgpos_dbg_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = GetEnvironmentVariableA("KITTY_WINPOS_DEBUG", NULL, 0) ? 1 : 0;
    return cached;
}
#ifdef KITTY_CFGBOX_ACTIVATION_TRACE
/*
 * NOT COMPILED INTO A RELEASE BUILD. Configure with
 * -DKITTY_CFGBOX_ACTIVATION_TRACE=ON to include it, then set
 * KITTY_CFGBOX_ACTIVATION_DEBUG=1 at run time to trace the config box's
 * ACTIVATION to %TEMP%\kitty_cfgbox_activation.log. Two switches on
 * purpose: the code stays in the tree for the next time a window loses its
 * place, and a shipped binary carries none of it.
 *
 * Kept in the source deliberately. hknet/KiTTY#38 - the box dropping behind
 * whatever window was behind it - was invisible to every outside measurement
 * and to reasoning alike; it was this trace that showed the box losing
 * WS_VISIBLE and the click landing on the window underneath. If a window of ours ever seems to lose its place again, turn
 * this on first: it names the window taking over, its process, where the
 * cursor was, and what was under it.
 */
static int kitty_cfgact_dbg_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = GetEnvironmentVariableA(
            "KITTY_CFGBOX_ACTIVATION_DEBUG", NULL, 0) ? 1 : 0;
    return cached;
}

static void kitty_cfgact_dbg(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    char path[MAX_PATH], oc[64] = "", ot[160] = "", fc[64] = "";
    char gc[64] = "", gt[160] = "", uc[64] = "";
    HWND other = (HWND)lParam, f, fg, under;
    DWORD opid = 0, gpid = 0, upid = 0, n;
    POINT cur = { 0, 0 };
    RECT box = { 0, 0, 0, 0 };
    SYSTEMTIME st;
    FILE *fp;

    if (!kitty_cfgact_dbg_enabled())
        return;
    n = GetTempPathA(sizeof(path), path);
    if (!n || n >= sizeof(path) - 40)
        return;
    strcat(path, "kitty_cfgbox_activation.log");
    fp = fopen(path, "a");
    if (!fp)
        return;

    f = GetFocus();
    if (f) GetClassNameA(f, fc, sizeof(fc));
    if (other) {
        GetClassNameA(other, oc, sizeof(oc));
        GetWindowTextA(other, ot, sizeof(ot));
        GetWindowThreadProcessId(other, &opid);
    }
    /* lParam is NULL whenever the window taking over belongs to another
     * thread, which is every interesting case - so ask the system who holds
     * the foreground, and what sits under the mouse. A cursor INSIDE our
     * rectangle with someone else's window under it means the click fell
     * through us, which is what #38 turned out to be. */
    fg = GetForegroundWindow();
    if (fg) {
        GetClassNameA(fg, gc, sizeof(gc));
        GetWindowTextA(fg, gt, sizeof(gt));
        GetWindowThreadProcessId(fg, &gpid);
    }
    GetCursorPos(&cur);
    GetWindowRect(hwnd, &box);
    under = WindowFromPoint(cur);
    if (under) {
        GetClassNameA(under, uc, sizeof(uc));
        GetWindowThreadProcessId(under, &upid);
    }
    GetLocalTime(&st);
    fprintf(fp,
            "%02d:%02d:%02d.%03d  %s  focus=%s(id=%d)  other=%s pid=%lu \"%s\"\n"
            "                       cursor (%ld,%ld) %s the box; under it: "
            "%s pid=%lu%s; WS_VISIBLE=%d\n"
            "                       foreground now: %s pid=%lu \"%s\"%s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LOWORD(wParam) ? "ACTIVE  " : "INACTIVE",
            fc, f ? GetDlgCtrlID(f) : -1, oc, (unsigned long)opid, ot,
            cur.x, cur.y,
            (cur.x >= box.left && cur.x < box.right &&
             cur.y >= box.top && cur.y < box.bottom) ? "INSIDE" : "outside",
            uc, (unsigned long)upid,
            upid == GetCurrentProcessId() ? " (ours)" : "",
            (GetWindowLong(hwnd, GWL_STYLE) & WS_VISIBLE) ? 1 : 0,
            gc, (unsigned long)gpid, gt,
            gpid == GetCurrentProcessId() ? "  <-- ours" : "");
    fclose(fp);
}
#else
#define kitty_cfgact_dbg(hwnd, wp, lp) ((void)0)
#endif /* KITTY_CFGBOX_ACTIVATION_TRACE */

static void kitty_cfgpos_dbg(const char *fmt, ...)
{
    if (!kitty_cfgpos_dbg_enabled()) return;
    char path[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(path), path);
    if (!n || n >= sizeof(path) - 20) return;
    strcat(path, "kitty_winpos.log");
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(fp, "%02d:%02d:%02d.%03d [cfgbox] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(fp, fmt, ap); va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}
static void kitty_cfgbox_save_pos(HWND hwnd)
{
    RECT r;
    if (!hwnd || IsIconic(hwnd) || IsZoomed(hwnd) || !GetWindowRect(hwnd, &r)) {
        kitty_cfgpos_dbg("SAVE skipped (iconic/zoomed/no-rect)");
        return;
    }
    char base[600];
    _snprintf(base, sizeof(base), "%s\\WindowPos", kitty_registry_base());
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, base, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        LONG xy[2]; xy[0] = r.left; xy[1] = r.top;
        RegSetValueExA(hk, "ConfigBox", 0, REG_BINARY, (const BYTE *)xy, sizeof(xy));
        RegCloseKey(hk);
        kitty_cfgpos_dbg("SAVE ok (%ld,%ld) -> HKCU\\%s [ConfigBox]", r.left, r.top, base);
    } else {
        kitty_cfgpos_dbg("SAVE FAILED RegCreateKeyEx HKCU\\%s", base);
    }
}
/*
 * Remember how big the user dragged the box.
 *
 * It is stored as [ConfigBox] windowheight/windowwidth - the same two keys the
 * fields on Application > Config window edit - so that dragging the window and
 * typing a size are one setting rather than two that can disagree. The values
 * are LOGICAL pixels, because that is what those keys have always meant and
 * what makes them portable between a scaled display and an unscaled one.
 *
 * The store itself is KiTTY's, and this file compiles into the stock variants
 * too, so it goes out through an accessor with a do-nothing stub.
 */
void kitty_cfgbox_store_size(int w, int h);   /* kitty_config.c / stub */

static void kitty_cfgbox_save_size(HWND hwnd)
{
    RECT r;
    HDC hdc;
    double sx, sy;

    /* Not while minimised or maximised: neither is a size the user chose for
     * the window to have, and storing one would make it the size the NEXT box
     * opens at. */
    if (!hwnd || IsIconic(hwnd) || IsZoomed(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    /*
     * ONLY when the drag that just ended actually changed the size.
     *
     * WM_EXITSIZEMOVE ends a MOVE as readily as a resize. Without this,
     * dragging the box across the screen writes windowheight/windowwidth for
     * the first time - silently turning someone who had no explicit size into
     * someone who has one, and an explicit windowheight is exactly what stops
     * [ConfigBox] height growing the window from then on. Moving a window is
     * not asking for its size to be remembered.
     */
    if ((r.right - r.left) == kitty_cfg_dragsize.cx &&
        (r.bottom - r.top) == kitty_cfg_dragsize.cy)
        return;
    hdc = GetDC(hwnd);
    if (!hdc)
        return;
    sx = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0;
    sy = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0;
    ReleaseDC(hwnd, hdc);
    if (sx <= 0 || sy <= 0)
        return;
    kitty_cfgbox_store_size((int)((r.right - r.left) / sx),
                            (int)((r.bottom - r.top) / sy));
}

static int kitty_cfgbox_restore_pos(HWND hwnd)
{
    char base[600];
    _snprintf(base, sizeof(base), "%s\\WindowPos", kitty_registry_base());
    LONG xy[2]; DWORD sz = sizeof(xy);
    LONG rc = RegGetValueA(HKEY_CURRENT_USER, base, "ConfigBox", RRF_RT_REG_BINARY,
                           NULL, xy, &sz);
    if (rc != ERROR_SUCCESS || sz != sizeof(xy)) {
        kitty_cfgpos_dbg("RESTORE no value (rc=%ld sz=%lu) HKCU\\%s", (long)rc, (unsigned long)sz, base);
        return 0;
    }
    POINT pt; pt.x = xy[0] + 8; pt.y = xy[1] + 8;
    int onmon = (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != NULL);
    if (!onmon) { kitty_cfgpos_dbg("RESTORE off-screen (%ld,%ld)", xy[0], xy[1]); return 0; }
    int ok = SetWindowPos(hwnd, NULL, xy[0], xy[1], 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ? 1 : 0;
    kitty_cfgpos_dbg("RESTORE setpos (%ld,%ld) -> %d", xy[0], xy[1], ok);
    return ok;
}

/* KiTTY: Ctrl+F anywhere in the config box jumps to the Session panel and
 * focuses the saved-sessions name box with its content fully selected, so
 * typing immediately starts a new list search (keyboard-first flow — without
 * this, getting back to the search from another panel takes a tree click plus
 * several Tabs). Keystrokes go to whichever child control has focus, so this
 * is a thread-scoped WH_KEYBOARD hook that lives only while a config box with
 * a session box exists; the stock GUI variants' stub accessor returns NULL,
 * so they never install it. */
dlgcontrol *kitty_config_session_filter_ctrl(void); /* kitty_config.c / stub */
bool kitty_red_caption(const char *text);  /* kitty_config.c / stub */
bool kitty_bold_caption(const char *text);           /* kitty_config.c / stub */
void kitty_cfgbox_workplace_poll(dlgparam *dp);      /* kitty_config.c / stub */

/* KiTTY: the loaded session's name, on the tab strip's row and centred over
 * the panel area. However deep in the tree the reader goes, whose settings
 * these are stays in sight - it is dialog furniture, so it never scrolls.
 * Text kept current alongside the one-second poll: a Load is the only thing
 * that changes it, and the poll already exists. */
static void kitty_cfg_session_label_update(struct dlgparam *dp)
{
    char buf[256], want[256];
    const char *sn;
    if (!kitty_cfg_session_label || !dp || !dp->data)
        return;
    sn = conf_get_str((Conf *)dp->data, CONF_sessionname);
    if (sn && *sn)
        _snprintf(want, sizeof(want) - 1, "Currently loaded session: %s", sn);
    else
        /* No loaded session IS quick connect - the dedicated mode
         * (loadlastsession=no) and the ad-hoc route (loading Default
         * Settings) both land in exactly this state, and there is no third
         * way to be here. */
        strcpy(want, "Quick Connect Mode active");
    want[sizeof(want) - 1] = '\0';
    buf[0] = '\0';
    GetWindowTextA(kitty_cfg_session_label, buf, sizeof(buf));
    if (strcmp(buf, want))
        SetWindowTextA(kitty_cfg_session_label, want);
}
const char *kitty_cfgbox_wanted_panel(void);         /* kitty_config.c / stub:
                                                      * panel path to open on,
                                                      * or NULL for the first */
#define KITTY_WORKPLACE_POLL_TIMER 8730
#define KITTY_PANEL_WARMUP_TIMER 8731
bool kitty_config_select_root_folder(dlgparam *dp); /* kitty_config.c / stub */
void kitty_config_end_folder_rename(dlgparam *dp);  /* kitty_config.c / stub */
/* Defined below, with the tree builder it needs: the Ctrl+F jump has to be
 * able to get back to the Session tab, and the hook that calls it is written
 * before either exists. */
static void kitty_cfg_goto_session_panel(void);
static HHOOK kitty_cfg_kbdhook = NULL;
static HWND kitty_cfg_hwnd = NULL;

/* The active panel's own help topic: the first control on it that carries
 * one (no_help is NULL, so those skip themselves). NULL when the asking
 * window is not the config box, which sends the WM_HELP fallback to the
 * manual's front page instead. */
static const char *kitty_cfg_panel_helpctx(HWND hwnd)
{
    if (hwnd != kitty_cfg_hwnd || !kitty_cfg_active_panel)
        return NULL;
    for (size_t i = 0; i < kitty_cfg_active_panel->nctrls; i++) {
        struct winctrl *c = kitty_cfg_active_panel->ctrls[i];
        if (c->ctrl && c->ctrl->helpctx)
            return c->ctrl->helpctx;
    }
    return NULL;
}
static HWND kitty_cfg_treeview = NULL;
static HTREEITEM kitty_cfg_sessionitem = NULL;
static dlgparam *kitty_cfg_dp = NULL;

/*
 * The other direction from kitty_cfgbox_save_size: a size was TYPED on
 * Application > Config window, so the box that panel lives in takes it now
 * rather than the next one. Called from that panel's handler.
 */
void kitty_cfgbox_apply_size(void)
{
    extern int GetConfigBoxWindowHeight(void);
    extern int GetConfigBoxWindowWidth(void);
    RECT r;
    HDC hdc;
    int w, h;
    double sx, sy;

    if (!kitty_cfg_hwnd || !kitty_cfg_layout_ready ||
        IsIconic(kitty_cfg_hwnd) || IsZoomed(kitty_cfg_hwnd) ||
        !GetWindowRect(kitty_cfg_hwnd, &r))
        return;
    hdc = GetDC(kitty_cfg_hwnd);
    if (!hdc)
        return;
    sx = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0;
    sy = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0;
    ReleaseDC(kitty_cfg_hwnd, hdc);

    w = GetConfigBoxWindowWidth();
    h = GetConfigBoxWindowHeight();
    /* A blank field means "not set", not "zero wide": keep what the window
     * has. Half-typed numbers arrive here too - "8" on the way to "800" - so
     * the same floor a drag is held to applies, and the box stops at its
     * minimum rather than collapsing while the number is still being typed. */
    {
        int askedw = w, askedh = h;        /* 0 = not set at all */
        int clampedw = 0, clampedh = 0;

        w = askedw > 0 ? (int)(askedw * sx) : (r.right - r.left);
        h = askedh > 0 ? (int)(askedh * sy) : (r.bottom - r.top);
        if (w < kitty_cfg_minsize.cx) { w = kitty_cfg_minsize.cx; clampedw = 1; }
        if (h < kitty_cfg_minsize.cy) { h = kitty_cfg_minsize.cy; clampedh = 1; }
        if (w != r.right - r.left || h != r.bottom - r.top)
            SetWindowPos(kitty_cfg_hwnd, NULL, 0, 0, w, h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        /*
         * Say what was actually applied - but ONLY for a dimension that was
         * both asked for and refused.
         *
         * The box cannot go below the size its own template produced, so a
         * smaller number is clamped, and a field still holding that number
         * reads as the setting being ignored. Writing the real size back puts
         * the two into agreement. Only this side knows the minimum, which is
         * why the correction happens here rather than where it was typed.
         *
         * Writing BOTH would be a different bug: typing a height would pin
         * the width as well, turning "no width set" into a stored one behind
         * the user's back - the same mistake as a move-only drag recording a
         * size.
         */
        if (clampedw || clampedh)
            kitty_cfgbox_store_size(clampedw ? (int)(w / sx) : 0,
                                    clampedh ? (int)(h / sy) : 0);
    }
}

static LRESULT CALLBACK kitty_cfg_kbd_hookproc(int code, WPARAM wParam,
                                               LPARAM lParam)
{
    if (code == HC_ACTION && (wParam == 'F' || wParam == 'G') &&
        !(lParam & 0x80000000) &&               /* key-down only */
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) &&
        kitty_cfg_hwnd && kitty_cfg_dp) {
        HWND focus = GetFocus();
        if (focus &&
            (focus == kitty_cfg_hwnd || IsChild(kitty_cfg_hwnd, focus))) {
            dlgcontrol *ctrl = kitty_config_session_filter_ctrl();
            if (ctrl) {
                /* Ctrl+G = "search everywhere": clear the folder filter back
                 * to the root list first, then do exactly what Ctrl+F does.
                 * Ctrl+F on its own only searches the selected folder. */
                if (wParam == 'G')
                    kitty_config_select_root_folder(kitty_cfg_dp);
                /* Both keys: asking to search ends a pending folder rename, so
                 * the box they focus is one that actually filters. */
                kitty_config_end_folder_rename(kitty_cfg_dp);
                /* The session list lives on the Session tab, so get there
                 * first. A stored tree ITEM cannot be used for this any more:
                 * switching tabs deletes every item in the tree, so the one
                 * recorded when the box opened may long since have been freed.
                 * The PATH survives, and finding it also tells us whether the
                 * right tab is already showing. */
                kitty_cfg_goto_session_panel();
                dlg_set_focus_later(ctrl, kitty_cfg_dp);
                return 1;               /* handled: swallow the keystroke */
            }
        }
    }
    return CallNextHookEx(kitty_cfg_kbdhook, code, wParam, lParam);
}

/*
 * Which tab a panel belongs to, and the tree for one of them.
 *
 * The split is a prefix test on the path: everything under "Application/" is
 * the Application tab, everything else is the Session tab. Paths are already
 * the way panels are addressed everywhere else (the cache is keyed by them,
 * ctrl_find_path looks them up), so the two trees are one walk with a filter
 * rather than a second data structure.
 *
 * The prefix is STRIPPED when the item is inserted: under a tab already called
 * Application, a root item repeating the word says nothing. That is why this
 * cannot simply call the stock loop - it inserts at the path's own depth.
 */
#define KITTY_APPTAB_PREFIX "Application/"

/*
 * A panel that can refuse to be left.
 *
 * Every other panel writes straight into the session's Conf, so leaving one is
 * free and nothing is ever lost. A panel that edits something ELSE - the
 * named-proxy definitions, which are only written when Save is pressed - has
 * work in hand that switching away would silently discard. It registers a
 * guard here; the guard returns false to keep the user where they are.
 *
 * Asked BEFORE the change, from TVN_SELCHANGING and TCN_SELCHANGING, because
 * a tree selection cannot be taken back afterwards: by the time SELCHANGED
 * arrives the old panel is already going.
 */
static bool (*kitty_cfg_leave_guard)(void) = NULL;

void kitty_cfg_set_leave_guard(bool (*fn)(void))
{
    kitty_cfg_leave_guard = fn;
}

static bool kitty_cfg_may_leave(void)
{
    return kitty_cfg_leave_guard ? kitty_cfg_leave_guard() : true;
}

/*
 * True while the tree is being emptied and refilled.
 *
 * Emptying a tree does not simply clear the selection: it MOVES it, item by
 * item, and every move is an ordinary selection-change notification carrying a
 * real path. Left alone, a tab switch therefore ran the whole panel machinery
 * once per session panel - visibly fast-forwarding through the entire tree
 * before landing where it was going. Nothing about those intermediate
 * selections is a user's choice, so the handler ignores them outright; the one
 * selection that matters is made deliberately when the refill is done.
 */
static bool kitty_cfg_tree_rebuilding = false;

/*
 * Where the user was on each tab, so switching back returns there instead of
 * dropping them at the top. Index 0 is Session, 1 is Application. The strings
 * are the panels' own paths, which live as long as the dialog does.
 */
static const char *kitty_cfg_tab_last[2] = { NULL, NULL };

/*
 * The Application tab's leaf, remembered across configuration WINDOWS.
 *
 * Only that tab. The Session tab opens where it always has - on the session
 * panel, which is what most people came for - and moving that would be a
 * change to something everybody uses. The Application tab has no such obvious
 * first stop, and someone who was setting up proxies wants to be back among
 * proxies next time.
 *
 * In kitty.ini rather than in memory, because "next time" usually means the
 * next KiTTY rather than the next window of this one.
 */
static bool kitty_cfg_path_is_app(const char *path);   /* defined below */

int WriteParameter(const char *key, const char *name, char *value);   /* kitty.c */
int ReadParameterN(const char *key, const char *name, char *value, size_t size);
/*
 * Weak defaults: this file is in libguiterminal, which stock putty.exe and
 * pterm link WITHOUT kitty.c. They never reach the code below - there is no
 * Application tab in those builds - but the symbols still have to resolve.
 * With kitty.c present its strong definitions win.
 */
__attribute__((weak))
int WriteParameter(const char *key, const char *name, char *value)
{
    (void)key; (void)name; (void)value;
    return 0;
}
__attribute__((weak))
int ReadParameterN(const char *key, const char *name, char *value, size_t size)
{
    (void)key; (void)name; (void)size;
    if (value) value[0] = '\0';
    return 0;
}

static void kitty_cfg_remember_app_panel(const char *path)
{
    if (path && kitty_cfg_path_is_app(path))
        WriteParameter("ConfigBox", "applicationpanel", (char *)path);
}

static const char *kitty_cfg_remembered_app_panel(void)
{
    static char buf[256];
    buf[0] = '\0';
    if (!ReadParameterN("ConfigBox", "applicationpanel", buf, sizeof(buf)))
        return NULL;
    return buf[0] ? buf : NULL;
}

/* The tree item carrying a given path, or NULL. Walks children and siblings
 * rather than the visible list: an unexpanded branch is still a place the user
 * can have been. */
static HTREEITEM kitty_cfg_find_item(HWND tv, HTREEITEM from, const char *path)
{
    HTREEITEM it;
    for (it = from; it; it = TreeView_GetNextSibling(tv, it)) {
        TVITEM ti;
        HTREEITEM kid, hit;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TVIF_PARAM;
        ti.hItem = it;
        if (TreeView_GetItem(tv, &ti) && ti.lParam &&
            !strcmp((const char *)ti.lParam, path))
            return it;
        kid = TreeView_GetChild(tv, it);
        if (kid && (hit = kitty_cfg_find_item(tv, kid, path)) != NULL)
            return hit;
    }
    return NULL;
}

static bool kitty_cfg_path_is_app(const char *path)
{
    return !strncmp(path, KITTY_APPTAB_PREFIX, strlen(KITTY_APPTAB_PREFIX));
}

/* Build the tree for one tab. Returns the first path inserted, or NULL when
 * the tab has no panels at all - which is a real case: the Application tab is
 * empty mid-session, and an empty tab must not leave the box showing whatever
 * the other tab last had. */
/* Remembered category folds (kitty_config.c): what the user changed BY HAND
 * beats categoryexpand's default in BOTH directions - a collapsed node stays
 * collapsed, an explicitly expanded one stays expanded. Only the deviations
 * are stored, keyed by the PATH each tree item already carries in its lParam
 * - the path is the identifier, so a display rename cannot orphan a
 * remembered fold. */
extern void kitty_cfgtree_set_fold(const char *path, int expanded,
                                   int default_expanded);
extern int kitty_cfgtree_get_fold(const char *path);
extern void kitty_cfgtree_folds_save(void);

static void kitty_cfg_tree_walk_folds(HWND tree, HTREEITEM it,
                                      int level, bool apply)
{
    for (; it; it = TreeView_GetNextSibling(tree, it)) {
        TVITEM tvi;
        tvi.mask = TVIF_PARAM | TVIF_STATE | TVIF_HANDLE | TVIF_CHILDREN;
        tvi.hItem = it;
        tvi.stateMask = TVIS_EXPANDED;
        if (TreeView_GetItem(tree, &tvi) && tvi.cChildren > 0 && tvi.lParam) {
            const char *path = (const char *)tvi.lParam;
            /* What categoryexpand would do to this node: its CHILDREN sit
             * one level down, and treeview_insert expands a parent while
             * their level is within the configured depth. */
            int def_expanded = (level + 1 <= kitty_category_expand_depth);
            if (apply) {
                int o = kitty_cfgtree_get_fold(path);
                if (o == 0)
                    TreeView_Expand(tree, it, TVE_COLLAPSE);
                else if (o == 1)
                    TreeView_Expand(tree, it, TVE_EXPAND);
            } else {
                kitty_cfgtree_set_fold(path, !!(tvi.state & TVIS_EXPANDED),
                                       def_expanded);
            }
        }
        kitty_cfg_tree_walk_folds(tree, TreeView_GetChild(tree, it),
                                  level + 1, apply);
    }
}

/* Read the current fold state of every expandable node into the remembered
 * set. Called wherever the tree's items are about to go away - the tab-switch
 * rebuild empties the tree, and WM_DESTROY takes the window with it. */
static void kitty_cfg_tree_remember_collapsed(HWND tree)
{
    if (tree)
        kitty_cfg_tree_walk_folds(tree, TreeView_GetRoot(tree), 0, false);
}

static void kitty_cfg_tree_apply_collapsed(HWND tree)
{
    kitty_cfg_tree_walk_folds(tree, TreeView_GetRoot(tree), 0, true);
}

static const char *kitty_cfg_build_tree(PortableDialogStuff *pds,
                                        struct treeview_faff *faff,
                                        bool apptab)
{
    const char *first = NULL;
    char *path = NULL;
    int i;

    /*
     * Refill with drawing off: switching back to Session re-inserts 44 items,
     * and each insertion repaints on its own otherwise.
     *
     * WM_SETREDRAW is safe HERE and is not elsewhere: on a top-level window it
     * clears WS_VISIBLE while leaving hit-testing behind, which is how clicks
     * once fell through the configuration box to whatever was underneath
     * (hknet/KiTTY#38). This is a child control refilling its own contents,
     * which is the case the message is for.
     */
    kitty_cfg_tree_rebuilding = true;
    /* The rebuild is about to throw the items away; keep their expand state. */
    kitty_cfg_tree_remember_collapsed(faff->treeview);
    SendMessage(faff->treeview, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(faff->treeview);
    memset(faff->lastat, 0, sizeof(faff->lastat));

    for (i = 0; i < pds->ctrlbox->nctrlsets; i++) {
        struct controlset *cs = pds->ctrlbox->ctrlsets[i];
        const char *shown;
        char *c;
        int j;

        if (!cs->pathname[0])
            continue;
        if (kitty_cfg_path_is_app(cs->pathname) != apptab)
            continue;

        /* Depth is counted within the tab, so the Application tab's own
         * prefix does not push everything one level in. */
        shown = cs->pathname;
        if (apptab)
            shown += strlen(KITTY_APPTAB_PREFIX);

        j = path ? ctrl_path_compare(shown, path) : 0;
        if (j == INT_MAX)
            continue;                  /* same path, nothing to add */

        c = strrchr(shown, '/');
        if (!c)
            c = (char *)shown;
        else
            c++;

        treeview_insert(faff, j, c, cs->pathname);
        if (!first)
            first = cs->pathname;
        path = (char *)shown;
    }
    /* A remembered collapse beats categoryexpand's default. */
    kitty_cfg_tree_apply_collapsed(faff->treeview);
    SendMessage(faff->treeview, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(faff->treeview, NULL, TRUE);
    kitty_cfg_tree_rebuilding = false;
    return first;
}


/*
 * Show the Session tab's first panel, whatever tab is up.
 *
 * Used by the Ctrl+F/Ctrl+G jump, which wants the saved-session box on the
 * Session panel. Needs the dialog's PortableDialogStuff to rebuild the tree,
 * which the keyboard hook does not have - so the box records it while it is
 * open, next to the other things the hook uses.
 */
static PortableDialogStuff *kitty_cfg_pds = NULL;

/*
 * A setting that decides a control's SIZE has changed, so the panel holding
 * that control has to be laid out again - refreshing it would only re-read
 * values into controls that are already the wrong shape. Today that is the
 * saved-session list's row count ([ConfigBox] height) and the Session panel.
 *
 * Safe from the Config window panel because the Session panel is a different
 * one: a panel cannot be rebuilt while it is the one on screen, and this is
 * only ever reached from a field on another panel.
 */
void kitty_cfgbox_relayout_panel(const char *path)
{
    if (kitty_cfg_pds && path)
        (void)kitty_cfg_panel_relayout(kitty_cfg_pds, path);
}

/*
 * The window a control was built into, found from the dlgcontrol itself.
 *
 * The panel that declares a control owns what it wants done with it; this
 * owns the lookup, which is the only part needing the winctrls trees. Used by
 * the saved-session panel to spread its button column down the list beside
 * it - see kitty_config_session_distribute().
 */
HWND kitty_cfg_ctrl_hwnd(dlgcontrol *ctrl)
{
    struct winctrl *c;

    if (!ctrl || !kitty_cfg_pds || !kitty_cfg_panel_host)
        return NULL;
    c = winctrl_findbyctrl(&kitty_cfg_pds->ctrltrees[TREE_PANEL], ctrl);
    if (!c)
        return NULL;
    /*
     * The FIRST id that has a window, not base_id.
     *
     * A winctrl reserves a block of ids and does not necessarily use all of
     * them: a list box declared with no label still accounts for the label,
     * so base_id names a static that was never created and GetDlgItem on it
     * answers NULL - which is how the saved-session list came back as "no
     * such window" while its dlgcontrol was perfectly valid.
     */
    for (int k = 0; k < c->num_ids; k++) {
        HWND h = GetDlgItem(kitty_cfg_panel_host, c->base_id + k);
        if (h)
            return h;
    }
    return NULL;
}

/*
 * The window a modal raised from the configuration box should sit on.
 *
 * MessageBox centres itself on its OWNER, and on the screen when it has none -
 * which on a wide monitor puts the question somewhere the user is not looking,
 * and leaves it behind the window that asked it. The box records its own
 * handle while it is open; anything raised from a panel asks here for it.
 *
 * Falls back to the active window rather than to NULL: outside the
 * configuration box (a session window, the launcher) that is still better than
 * the middle of the screen.
 */
HWND kitty_cfg_modal_owner(void)
{
    if (kitty_cfg_hwnd && IsWindow(kitty_cfg_hwnd))
        return kitty_cfg_hwnd;
    return GetActiveWindow();
}

void kitty_cfg_goto_panel(const char *path)
{
    HWND tv = kitty_cfg_treeview;
    HWND strip;
    HTREEITEM want;
    bool apptab;

    if (!tv || !kitty_cfg_hwnd || !kitty_cfg_pds || !path)
        return;
    apptab = kitty_cfg_path_is_app(path);
    strip = GetDlgItem(kitty_cfg_hwnd, IDCX_TABSTRIP);
    if (strip && (SendMessage(strip, TCM_GETCURSEL, 0, 0) != 0) != apptab) {
        /*
         * Record where the user was on the tab being LEFT, exactly as the tab
         * strip itself does. Jumping from a session panel to the Application
         * tab - which is what "Edit named proxies..." does - otherwise lost
         * the session panel it came from, so going back landed on Session
         * while switching tabs by hand remembered properly. Same feature, two
         * roads in, and only one of them was doing the bookkeeping.
         */
        if (kitty_cfg_active_panel)
            kitty_cfg_tab_last[apptab ? 0 : 1] = kitty_cfg_active_panel->path;
        struct treeview_faff faff;
        memset(&faff, 0, sizeof(faff));
        faff.treeview = tv;
        SendMessage(strip, TCM_SETCURSEL, apptab ? 1 : 0, 0);
        kitty_cfg_build_tree(kitty_cfg_pds, &faff, apptab);
    }
    want = kitty_cfg_find_item(tv, TreeView_GetRoot(tv), path);
    if (!want)
        want = TreeView_GetRoot(tv);
    if (want)
        TreeView_SelectItem(tv, want);
}

/* The Session tab's first panel: where the saved-session box is, and so where
 * the Ctrl+F/Ctrl+G jump has to land. */
static void kitty_cfg_goto_session_panel(void)
{
    HWND tv = kitty_cfg_treeview;
    HWND strip;

    if (!tv || !kitty_cfg_hwnd || !kitty_cfg_pds)
        return;
    strip = GetDlgItem(kitty_cfg_hwnd, IDCX_TABSTRIP);
    if (strip && SendMessage(strip, TCM_GETCURSEL, 0, 0) != 0) {
        struct treeview_faff faff;
        memset(&faff, 0, sizeof(faff));
        faff.treeview = tv;
        SendMessage(strip, TCM_SETCURSEL, 0, 0);
        kitty_cfg_build_tree(kitty_cfg_pds, &faff, false);
    }
    {
        HTREEITEM want = TreeView_GetRoot(tv);
        if (want)
            TreeView_SelectItem(tv, want);
    }
}

static INT_PTR GenericMainDlgProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam, void *ctx)
{
    PortableDialogStuff *pds = (PortableDialogStuff *)ctx;
    const int DEMO_SCREENSHOT_TIMER_ID = 1230;
    HWND treeview;
    struct treeview_faff tvfaff;

    switch (msg) {
      case WM_CTLCOLORSTATIC: {
        /*
         * KiTTY: draw the proxy-override caption BOLD AND RED while an override
         * is armed.
         *
         * Identified by its TEXT, not by a control id: the caption is set at
         * runtime by the handler in kitty_config.c, and plumbing an id out through
         * the portable control layer to reach it here would be a lot of machinery
         * for one label. kitty_red_caption() is the only thing that knows
         * which wording counts, and it lives beside the code that produces it.
         *
         * Stubbed to false in windows/kitty_config_stubs.c, so the stock variants -
         * which have no proxy override - link and behave exactly as before.
         */
        char buf[128];
        if (GetWindowTextA((HWND)lParam, buf, sizeof(buf)) <= 0)
            buf[0] = '\0';
        if (buf[0] && kitty_red_caption(buf)) {
            static HFONT bold = NULL;      /* built once, reused for the process */
            HDC hdc = (HDC)wParam;
            if (!bold) {
                LOGFONT lf;
                HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
                if (cur && GetObject(cur, sizeof(lf), &lf)) {
                    lf.lfWeight = FW_BOLD;
                    bold = CreateFontIndirect(&lf);
                }
            }
            if (bold)
                SelectObject(hdc, bold);
            /* Dark red, not pure red: it stays legible on the grey dialog face
             * and on the lighter face high-contrast themes use. */
            SetTextColor(hdc, RGB(192, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        /* KiTTY: captions that are BOLD but not coloured - the workplace-proxy
         * box, which is the one thing on that panel that does not belong to the
         * session being configured. Same text-matching contract as above; a
         * theme-drawn group box may ignore this, which is why the box also
         * carries a bold lead line of its own. */
        if (buf[0] && kitty_bold_caption(buf)) {
            static HFONT boldplain = NULL;
            HDC hdc = (HDC)wParam;
            if (!boldplain) {
                LOGFONT lf;
                HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
                if (cur && GetObject(cur, sizeof(lf), &lf)) {
                    lf.lfWeight = FW_BOLD;
                    boldplain = CreateFontIndirect(&lf);
                }
            }
            if (boldplain)
                SelectObject(hdc, boldplain);
            SetBkMode(hdc, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      }
      /*
       * KiTTY: the resizable box. WM_SIZE re-places the outer furniture
       * against the baseline captured at the end of creation; the guard
       * inside kitty_cfg_layout_relayout is what lets this message arrive
       * during creation, before there is anything to place.
       */
      case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            kitty_cfg_layout_relayout(hwnd);
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_GETMINMAXINFO:
        /* The template size is the floor. Without this the box can be dragged
         * to nothing, the growth goes negative, and every anchored control is
         * placed at a negative size - which is also what
         * scripts/qa_window_minsize.ps1 asks every window to do. */
        if (kitty_cfg_layout_ready) {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            mmi->ptMinTrackSize.x = kitty_cfg_minsize.cx;
            mmi->ptMinTrackSize.y = kitty_cfg_minsize.cy;
            return 0;
        }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_ENTERSIZEMOVE: {
        /* The size to compare against when the drag ends: a drag that only
         * MOVED the box must not be taken for a request to remember a size. */
        RECT r;
        if (GetWindowRect(hwnd, &r)) {
            kitty_cfg_dragsize.cx = r.right - r.left;
            kitty_cfg_dragsize.cy = r.bottom - r.top;
        }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      }
      case WM_EXITSIZEMOVE: {
        /*
         * A WIDTH change means every panel has to be laid out again.
         *
         * A panel's controls are positioned when the panel is BUILT, from the
         * host's width at that moment, so widening the box moves the host and
         * leaves the contents at their old width - clipped, and obviously so.
         * Height needs none of this: the panel area scrolls.
         *
         * At the END of the drag, not per WM_SIZE: re-laying 44 panels out on
         * every mouse-move would be unusable. The cached-but-hidden panels are
         * done too, or the first visit after a resize shows the old width.
         */
        RECT now;
        if (kitty_cfg_layout_ready && GetWindowRect(hwnd, &now) &&
            (now.right - now.left) != kitty_cfg_dragsize.cx && kitty_cfg_pds) {
            for (size_t i = 0; i < kitty_cfg_npanels; i++)
                kitty_cfg_panel_relayout_ex(
                    kitty_cfg_pds, kitty_cfg_panels[i],
                    kitty_cfg_panels[i] == kitty_cfg_active_panel);
        }
        kitty_cfgbox_save_pos(hwnd);   /* remember where the user dragged it */
        kitty_cfgbox_save_size(hwnd);  /* ... and how big they dragged it */
        /*
         * If the size fields are the thing on screen, they are now stale: the
         * drag wrote the very setting they display. Refreshing the panel is
         * how they are made to agree, and it is confined to that one panel
         * because EVENT_REFRESH overwrites what a control holds - which on a
         * panel with unsaved edits (the named-proxy editor) would throw them
         * away.
         */
        if (kitty_cfg_active_panel && kitty_cfg_dp &&
            !strcmp(kitty_cfg_active_panel->path, "Application/Config window"))
            kitty_cfg_panel_refresh(kitty_cfg_dp, kitty_cfg_active_panel);
      }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_DESTROY:
        /* The tree dies with the dialog: keep its expand state first, and
         * write the remembered collapses back. */
        kitty_cfg_tree_remember_collapsed(kitty_cfg_treeview);
        kitty_cfgtree_folds_save();
        /* Robust backstop: capture the final position at close, regardless of
         * how the box was moved (WM_EXITSIZEMOVE only fires on interactive drag). */
        kitty_cfgbox_save_pos(hwnd);
        /* BEFORE the cache is reset: closing while on an Application panel
         * counts as having been there, and the reset is what forgets which
         * panel that was. */
        if (kitty_cfg_active_panel)
            kitty_cfg_remember_app_panel(kitty_cfg_active_panel->path);
        kitty_cfg_panel_cache_reset();  /* the windows die with the dialog */
        kitty_cfg_layout_free();        /* so does the layout baseline */
        /* KiTTY: tear down the Ctrl+F session-search jump with its dialog. */
        if (kitty_cfg_hwnd == hwnd) {
            if (kitty_cfg_kbdhook) {
                UnhookWindowsHookEx(kitty_cfg_kbdhook);
                kitty_cfg_kbdhook = NULL;
            }
            kitty_cfg_hwnd = NULL;
            kitty_cfg_treeview = NULL;
            kitty_cfg_session_label = NULL;
            kitty_cfg_pds = NULL;
            kitty_cfg_sessionitem = NULL;
            kitty_cfg_dp = NULL;
        }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_INITDIALOG: {
        pds_initdialog_start(pds, hwnd);

        /*
         * NO WS_CLIPCHILDREN here, however tempting it looks.
         *
         * It stops the dialog painting the background under its own controls,
         * which does reduce the flash behind a scroll - and it leaves the
         * PREVIOUS panel visible inside the new one's group boxes. A group box
         * draws its frame and caption and nothing else: its interior is
         * transparent, and the parent is what fills it. Clip the parent away
         * and whatever was on those pixels stays there, so switching panels
         * left the old panel's text showing through the new panel's boxes.
         */

        /*
         * KiTTY: the saved-session list height is configurable via kitty.ini
         * [ConfigBox] height (GetConfigBoxHeight(), default 16 = stock fit).
         * A taller list needs a taller window; cb_extra_du is how much taller,
         * in dialog units (0 at or below the stock-fit height).
         *
         * It is no longer built INTO the layout. Everything below is created
         * at the template size, and this - like [ConfigBox] windowheight, and
         * like a drag of the window's own edge - is applied afterwards as a
         * resize, which the WM_SIZE relayout then follows. There is one layout
         * path now, so the creation-time geometry and the resize-time geometry
         * cannot disagree with each other.
         */
        int cb_extra_du = 0;
        {
            extern int GetConfigBoxHeight(void);
            extern int kitty_proxy_choice_shown(void);
            int extra_rows = GetConfigBoxHeight() - 16;  /* 16 = stock-fit rows */
            if (extra_rows > 0) cb_extra_du = extra_rows * 8;  /* ~8 du / list row */
            /* The Proxy-choice droplist, when shown, adds a row to the Session
             * panel that the list-height estimate above doesn't cover. */
            if (kitty_proxy_choice_shown()) cb_extra_du += 9;
            /* Safety margin so the bottom checkbox frame never rides under the
             * button row (the per-row estimate can fall a little short, esp. at
             * higher DPI). */
            if (cb_extra_du > 0) cb_extra_du += 9;
        }

        pds_create_controls(pds, TREE_BASE, IDCX_STDBASE, 3, 3,
                            CFGBOX_BUTTONROW_DU, "");   /* buttons row */
        /* Where that row landed, in pixels: the floor of the panel area, and
         * the line nothing scrolled may cross - you shall not pass! Taken
         * from the same units the row was created with, so the two cannot
         * disagree. Kept up to date by the relayout as the row moves. */
        {
            RECT br = { 0, CFGBOX_BUTTONROW_DU, 0, 0 };
            MapDialogRect(hwnd, &br);
            kitty_cfg_buttonrow_top = br.top;
        }

        /*
         * The panel HOST, created before any panel exists: every panel's
         * controls are built into it. It covers the panel area exactly, so
         * Windows clips them to it and the button strip below cannot be
         * drawn into whatever a panel does.
         */
        {
            RECT area;
            int barw = GetSystemMetrics(SM_CXVSCROLL);
            kitty_cfg_panel_rect(hwnd, &area);
            kitty_cfg_panel_host =
                CreateDialog(hinst, MAKEINTRESOURCE(IDD_PANELHOST),
                             hwnd, PanelHostProc);
            if (kitty_cfg_panel_host) {
                /*
                 * The host stops short of the right edge by the width of a
                 * scroll bar, so the bar sits BESIDE it and not under it.
                 * Overlapping them made the bar unclickable: the host is a
                 * window like any other, it was over the bar, and every click
                 * went to the host - the wheel still worked, which is exactly
                 * how it looked from the outside.
                 *
                 * The gap is kept whether or not a bar is showing, so no
                 * panel is re-laid out when one appears.
                 */
                SetWindowPos(kitty_cfg_panel_host, HWND_BOTTOM,
                             area.left, area.top,
                             (area.right - area.left) - barw,
                             area.bottom - area.top,
                             SWP_NOACTIVATE);
                /* The dialog's own font, or panels are laid out in one font
                 * and drawn in another. */
                SendMessage(kitty_cfg_panel_host, WM_SETFONT,
                            SendMessage(hwnd, WM_GETFONT, 0, 0),
                            MAKELPARAM(FALSE, 0));
                ShowWindow(kitty_cfg_panel_host, SW_SHOW);
            }
        }

        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)));

        /* The sizing, the placement and the foreground dance all used to sit
         * here. They now follow the tree, because the tree is the last piece
         * of outer furniture and the layout baseline cannot be captured until
         * every piece of it exists. */

        /*
         * Create the tree view.
         */
        {
            RECT r;
            WPARAM font;
            HWND tabstrip;

            /*
             * The tab strip, where the "Cate&gory:" label used to be. The
             * label said what the tree is; the tabs say that and which of the
             * two trees is showing, so keeping both would spend height twice
             * to say one thing.
             */
            r.left = 3;
            r.right = r.left + 95;
            r.top = 3;
            r.bottom = r.top + CFGBOX_TABSTRIP_DU;
            MapDialogRect(hwnd, &r);
            tabstrip = CreateWindowEx(0, WC_TABCONTROL, "",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      TCS_FOCUSNEVER,
                                      r.left, r.top,
                                      r.right - r.left, r.bottom - r.top,
                                      hwnd, (HMENU) IDCX_TABSTRIP, hinst,
                                      NULL);
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(tabstrip, WM_SETFONT, font, MAKELPARAM(true, 0));
            {
                TCITEM ti;
                memset(&ti, 0, sizeof(ti));
                ti.mask = TCIF_TEXT;
                ti.pszText = (char *)"Session";
                SendMessage(tabstrip, TCM_INSERTITEM, 0, (LPARAM)&ti);
                ti.pszText = (char *)"Application";
                SendMessage(tabstrip, TCM_INSERTITEM, 1, (LPARAM)&ti);
            }

            /* The session-name label shares the strip's row; see
             * kitty_cfg_session_label_update for what it shows. Centred over
             * the panel area, which is where the settings it names are. */
            {
                RECT area, band;
                kitty_cfg_panel_rect(hwnd, &area);
                band.left = 0; band.top = 3;
                band.right = 4; band.bottom = 3 + CFGBOX_TABSTRIP_DU;
                MapDialogRect(hwnd, &band);
                /* Never into the panel area below: the label ends where the
                 * panels begin, or it sits on the first panel's title. */
                if (band.bottom > area.top - 1)
                    band.bottom = area.top - 1;
                kitty_cfg_session_label = CreateWindowEx(
                    0, "STATIC", "",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                    area.left, band.top,
                    area.right - area.left, band.bottom - band.top,
                    hwnd, (HMENU) NULL, hinst, NULL);
                SendMessage(kitty_cfg_session_label, WM_SETFONT, font,
                            MAKELPARAM(true, 0));
                kitty_cfg_session_label_update(pds->dp);
            }

            r.left = 3;
            r.right = r.left + 95;
            r.top = 3 + CFGBOX_TABSTRIP_DU;
            r.bottom = r.top + CFGBOX_TREE_DU
                       - (CFGBOX_TABSTRIP_DU - 10); /* KiTTY: (was 219); gives
                                       * back what the tab strip took from the
                                       * old label. The tree rides the bottom
                                       * edge, so a taller box - however it got
                                       * that way - grows it from here. */
            MapDialogRect(hwnd, &r);
            treeview = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
                                      WS_CHILD | WS_VISIBLE |
                                      WS_TABSTOP | TVS_HASLINES |
                                      TVS_DISABLEDRAGDROP | TVS_HASBUTTONS
                                      | TVS_LINESATROOT |
                                      TVS_SHOWSELALWAYS, r.left, r.top,
                                      r.right - r.left, r.bottom - r.top,
                                      hwnd, (HMENU) IDCX_TREEVIEW, hinst,
                                      NULL);
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(treeview, WM_SETFONT, font, MAKELPARAM(true, 0));
            /* One item per step under the wheel; see KittyCfgTreeProc. */
            kitty_cfg_tree_oldproc = (WNDPROC)SetWindowLongPtr(
                treeview, GWLP_WNDPROC, (LONG_PTR)KittyCfgTreeProc);
            tvfaff.treeview = treeview;
            memset(tvfaff.lastat, 0, sizeof(tvfaff.lastat));
        }

        /*
         * KiTTY: the outer furniture is complete and still exactly the size
         * the template asked for. Capture that as the baseline - and as the
         * smallest the box may be dragged to - before anything resizes it.
         */
        kitty_cfg_layout_capture(pds, hwnd);

        /*
         * KiTTY: the configured size, applied as a RESIZE now that
         * there is a layout to follow it: an explicit [ConfigBox]
         * windowheight/windowwidth (pixels, DPI-scaled) if either is set,
         * otherwise the extra height a taller saved-session list needs.
         * Before placing the box, so it is placed at its final size.
         */
        {
            extern int GetConfigBoxWindowHeight(void);
            extern int GetConfigBoxWindowWidth(void);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int cur_w = wr.right - wr.left, cur_h = wr.bottom - wr.top;
            int want_w = cur_w, want_h = cur_h;
            int wh = GetConfigBoxWindowHeight(), ww = GetConfigBoxWindowWidth();
            HDC hdc = GetDC(hwnd);
            double sx = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0;
            double sy = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0;
            ReleaseDC(hwnd, hdc);
            if (wh > 0)
                want_h = (int)(wh * sy);
            else if (cb_extra_du > 0) {
                RECT er = { 0, 0, 0, cb_extra_du };
                MapDialogRect(hwnd, &er);
                want_h = cur_h + er.bottom;
            }
            if (ww > 0)
                want_w = (int)(ww * sx);
            /* Never below the template: the same floor WM_GETMINMAXINFO
             * enforces for a drag, applied to a stored value that might
             * predate a font change or come from a larger display. */
            if (want_w < kitty_cfg_minsize.cx) want_w = kitty_cfg_minsize.cx;
            if (want_h < kitty_cfg_minsize.cy) want_h = kitty_cfg_minsize.cy;
            if (want_w != cur_w || want_h != cur_h)
                SetWindowPos(hwnd, NULL, 0, 0, want_w, want_h,
                             SWP_NOMOVE | SWP_NOZORDER);
        }

        /* KiTTY: restore the remembered config-box position; centre if none/off-screen. */
        if (!kitty_cfgbox_restore_pos(hwnd))
            centre_window(hwnd);

        /* KiTTY: bring the startup configuration dialog to the front; it can
         * otherwise open behind already-open windows. The TOPMOST->NOTOPMOST
         * toggle forces it to the top of the Z-order even when Windows denies
         * SetForegroundWindow (foreground lock); SetForegroundWindow then also
         * activates it when the process has the foreground privilege. */
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);

        /*
         * Set up the tree view contents.
         */
        {
            HTREEITEM hfirst = NULL, hwanted = NULL;
            int i;
            char *path = NULL;
            char *firstpath = NULL, *wantedpath = NULL;

            for (i = 0; i < pds->ctrlbox->nctrlsets; i++) {
                struct controlset *s = pds->ctrlbox->ctrlsets[i];
                HTREEITEM item;
                int j;
                char *c;

                if (!s->pathname[0])
                    continue;
                /* The box opens on the Session tab, so its tree holds the
                 * session panels only; the Application ones arrive when that
                 * tab is chosen. */
                if (kitty_cfg_path_is_app(s->pathname))
                    continue;
                j = path ? ctrl_path_compare(s->pathname, path) : 0;
                if (j == INT_MAX)
                    continue;          /* same path, nothing to add to tree */

                /*
                 * We expect never to find an implicit path
                 * component. For example, we expect never to see
                 * A/B/C followed by A/D/E, because that would
                 * _implicitly_ create A/D. All our path prefixes
                 * are expected to contain actual controls and be
                 * selectable in the treeview; so we would expect
                 * to see A/D _explicitly_ before encountering
                 * A/D/E.
                 */
                assert(j == ctrl_path_elements(s->pathname) - 1);

                c = strrchr(s->pathname, '/');
                if (!c)
                    c = s->pathname;
                else
                    c++;

                item = treeview_insert(&tvfaff, j, c, s->pathname);
                if (!hfirst) {
                    hfirst = item;
                    firstpath = s->pathname;
                }
                /* KiTTY: open the box on a named panel instead of the first one,
                 * when something asked for that - the "your workplace proxy did
                 * not answer" notice opens the box straight at Connection/Proxy,
                 * where the switch, the proxy and the timeout all are. Recorded
                 * as the tree is built, because that is the only place the path
                 * and its tree item are known together. */
                if (kitty_cfgbox_wanted_panel() &&
                    !strcmp(s->pathname, kitty_cfgbox_wanted_panel())) {
                    hwanted = item;
                    wantedpath = s->pathname;
                }

                path = s->pathname;
            }

            /* A remembered collapse beats categoryexpand's default. (A wanted
             * panel inside a collapsed category still gets there: selecting
             * it below expands its parent chain, which is the explicit
             * navigation case.) */
            kitty_cfg_tree_apply_collapsed(tvfaff.treeview);

            /*
             * Put the treeview selection on to the first panel in the
             * ctrlbox - or on the panel something asked us to open on.
             */
            /* hfirst stays the FIRST panel (the Session one) because the Ctrl+F
             * jump below is anchored to it; only what we select changes. */
            HTREEITEM hsel = hwanted ? hwanted : hfirst;
            char *selpath = hwanted ? wantedpath : firstpath;

            /*
             * A wanted panel on the OTHER tab: the loop above only walked the
             * session paths, so it was never found. Switch the strip, build
             * that tab's tree, and take the item from there.
             */
            if (kitty_cfgbox_wanted_panel() &&
                kitty_cfg_path_is_app(kitty_cfgbox_wanted_panel())) {
                HWND strip = GetDlgItem(hwnd, IDCX_TABSTRIP);
                const char *first_app;
                SendMessage(strip, TCM_SETCURSEL, 1, 0);
                first_app = kitty_cfg_build_tree(pds, &tvfaff, true);
                if (first_app) {
                    HTREEITEM want = kitty_cfg_find_item(
                        treeview, TreeView_GetRoot(treeview),
                        kitty_cfgbox_wanted_panel());
                    if (!want)
                        want = TreeView_GetRoot(treeview);
                    hsel = want;
                    selpath = (char *)kitty_cfgbox_wanted_panel();
                    if (!kitty_cfg_find_item(treeview, TreeView_GetRoot(treeview),
                                             selpath))
                        selpath = (char *)first_app;
                }
            }
            TreeView_SelectItem(treeview, hsel);

            /* KiTTY: arm the Ctrl+F session-search jump (first tree item ==
             * the Session panel). Only when this dialog's ctrlbox actually
             * registered a session box — stock variants return NULL. */
            kitty_cfg_hwnd = hwnd;      /* the modal owner, always */
            if (kitty_config_session_filter_ctrl()) {
                kitty_cfg_hwnd = hwnd;
                kitty_cfg_dp = pds->dp;
                kitty_cfg_treeview = treeview;
                kitty_cfg_pds = pds;
                kitty_cfg_sessionitem = hfirst;
                kitty_cfg_kbdhook = SetWindowsHookEx(
                    WH_KEYBOARD, kitty_cfg_kbd_hookproc,
                    NULL, GetCurrentThreadId());
            }

            /*
             * And create the actual control set for that panel, to
             * match the initial treeview selection.
             */
            assert(selpath);     /* config.c must have given us _something_ */
            kitty_cfg_panel_cache_reset();  /* nothing may carry over */
            kitty_cfg_active_panel = kitty_cfg_panel_create(pds, selpath);
            dlg_refresh(NULL, pds->dp);    /* and set up control values */
        }

        /* KiTTY: the panel area's scroll bar. Created hidden and sized when a
         * panel that needs it is shown - see kitty_cfg_panel_scrollbar. It is
         * a child of the dialog rather than a WS_VSCROLL on the window,
         * because it scrolls the PANEL AREA and must not run the full height
         * of a window whose left third is the category tree. */
        CreateWindowEx(0, "SCROLLBAR", "",
                       WS_CHILD | SBS_VERT, 0, 0, 0, 0, hwnd,
                       (HMENU)(ULONG_PTR)IDCX_PANELSCROLL, hinst, NULL);
        if (kitty_cfg_active_panel) {
            kitty_cfg_active_panel->content_h =
                kitty_cfg_panel_measure(kitty_cfg_active_panel);
            kitty_cfg_panel_scrollbar(hwnd, kitty_cfg_active_panel, false);
        }

        if (dialog_box_demo_screenshot_filename)
            SetTimer(hwnd, DEMO_SCREENSHOT_TIMER_ID, TICKSPERSEC, NULL);

        /* KiTTY: workplace proxy mode is held by ANOTHER process (the
         * launcher), so switching it off from the tray reaches this box only if
         * the box looks. One second, one OpenFileMapping, and the poll repaints
         * two controls and only when the state actually moved. Stubbed to
         * nothing in the stock variants. */
        SetTimer(hwnd, KITTY_WORKPLACE_POLL_TIMER, 1000, NULL);

        /* KiTTY: warm the panel cache in the background - one hidden panel
         * per tick, so first visits cost a show, not a build. Not in demo-
         * screenshot mode, whose box exists only to be photographed once. */
        if (!dialog_box_demo_screenshot_filename)
            SetTimer(hwnd, KITTY_PANEL_WARMUP_TIMER, 120, NULL);

        pds_initdialog_finish(pds);
        return 0;
      }

      case WM_ACTIVATE:
        /* Traced only with KITTY_CFGBOX_ACTIVATION_DEBUG set; see
         * kitty_cfgact_dbg above for why this stayed in the source. */
        kitty_cfgact_dbg(hwnd, wParam, lParam);
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);

      /* KiTTY: the panel area's scroll bar. */
      case WM_VSCROLL:
        if (kitty_cfg_active_panel &&
            (HWND)lParam == GetDlgItem(hwnd, IDCX_PANELSCROLL)) {
            struct kitty_cfg_panel *p = kitty_cfg_active_panel;
            RECT area;
            int page, y = kitty_cfg_scroll_y;
            kitty_cfg_panel_rect(hwnd, &area);
            page = area.bottom - area.top;
            switch (LOWORD(wParam)) {
              case SB_LINEUP:        y -= 16;        break;
              case SB_LINEDOWN:      y += 16;        break;
              case SB_PAGEUP:        y -= page;      break;
              case SB_PAGEDOWN:      y += page;      break;
              case SB_TOP:           y = 0;          break;
              case SB_BOTTOM:        y = p->content_h; break;
              case SB_THUMBTRACK:
              case SB_THUMBPOSITION: {
                /* HIWORD(wParam) is 16-bit and a panel can be taller than
                 * that; ask the bar for the real position instead. */
                SCROLLINFO si;
                memset(&si, 0, sizeof(si));
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                if (GetScrollInfo((HWND)lParam, SB_CTL, &si))
                    y = si.nTrackPos;
                break;
              }
              default: return 0;
            }
            kitty_cfg_panel_scroll_to(hwnd, p, y);
            return 0;
        }
        return 0;

      case WM_MOUSEWHEEL: {
        /* Point at the tree and the wheel moves the TREE, whether or not it
         * has the focus. Windows sends the wheel to the focused window, so
         * without this the tree only ever scrolls after a click. */
        POINT wpt;
        HWND tv = GetDlgItem(hwnd, IDCX_TREEVIEW);
        wpt.x = (short)LOWORD(lParam);
        wpt.y = (short)HIWORD(lParam);
        if (tv) {
            RECT tr;
            GetWindowRect(tv, &tr);
            if (PtInRect(&tr, wpt)) {
                kitty_cfg_tree_wheel(tv, wParam);
                return 0;
            }
        }
      }
        /* The wheel scrolls the panel wherever the pointer is, as long as the
         * panel HAS a scroll bar - a panel that fits does not move under the
         * wheel, which is what the reader expects. */
        if (kitty_cfg_active_panel) {
            HWND sb = GetDlgItem(hwnd, IDCX_PANELSCROLL);
            if (sb && IsWindowVisible(sb)) {
                /*
                 * The remainder is KEPT. A mouse wheel sends WHEEL_DELTA per
                 * notch, but a trackpad sends a continuous stream of much
                 * smaller values: dividing each one by WHEEL_DELTA throws
                 * nearly all of them away, so the panel sits still and then
                 * jumps - "it is not scrolling, it is hopping". Accumulating
                 * turns the same stream into smooth movement, and a real
                 * notch still moves exactly one step.
                 */
                static int wheel_remainder = 0;
                int step = kitty_cfg_wheel_lines();
                int amount;
                wheel_remainder += GET_WHEEL_DELTA_WPARAM(wParam) * step;
                amount = wheel_remainder / WHEEL_DELTA;
                wheel_remainder -= amount * WHEEL_DELTA;
                if (amount)
                    kitty_cfg_panel_scroll_to(hwnd, kitty_cfg_active_panel,
                                              kitty_cfg_scroll_y - amount);
                return 0;
            }
        }
        return 0;
      case WM_TIMER:
        if ((UINT_PTR)wParam == KITTY_PANEL_WARMUP_TIMER) {
            if (!kitty_cfg_warmup_step(pds)) {
                KillTimer(hwnd, KITTY_PANEL_WARMUP_TIMER);
                /* Every panel exists now, so every setting has been through a
                 * handler that knows what it may hold. Anything unrepresentable
                 * found on the way is reported here - once, for the session as
                 * a whole. */
                kitty_conf_invalid_report(pds->dp, NULL);
            }
            return 0;
        }
        if ((UINT_PTR)wParam == KITTY_WORKPLACE_POLL_TIMER) {
            kitty_cfgbox_workplace_poll(pds->dp);
            kitty_cfg_session_label_update(pds->dp);
            return 0;
        }
        if (dialog_box_demo_screenshot_filename &&
            (UINT_PTR)wParam == DEMO_SCREENSHOT_TIMER_ID) {
            KillTimer(hwnd, DEMO_SCREENSHOT_TIMER_ID);
            char *err = save_screenshot(
                hwnd, dialog_box_demo_screenshot_filename);
            if (err) {
                MessageBox(hwnd, err, "Demo screenshot failure",
                           MB_OK | MB_ICONERROR);
                sfree(err);
            }
            ShinyEndDialog(hwnd, 0);
        }
        return 0;

      case WM_NOTIFY:
        /* Leaving a panel that has unsaved work: both roads out ask first, and
         * both are vetoable only BEFORE the fact. */
        if (((LOWORD(wParam) == IDCX_TREEVIEW &&
              ((LPNMHDR) lParam)->code == TVN_SELCHANGING) ||
             (LOWORD(wParam) == IDCX_TABSTRIP &&
              ((LPNMHDR) lParam)->code == TCN_SELCHANGING)) &&
            pds->initialised && !kitty_cfg_tree_rebuilding) {
            if (!kitty_cfg_may_leave()) {
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);  /* veto */
                return TRUE;
            }
        }
        /* A DOUBLE-CLICK on a tab header is a jump: the Session tab's goes to
         * the Session leaf, the Application tab's to Workplace proxy - the
         * two panels each tab most often exists to reach. A double-click on
         * the already-selected tab raises no TCN_SELCHANGE, so without this
         * it did nothing at all. */
        if (LOWORD(wParam) == IDCX_TABSTRIP &&
            ((LPNMHDR) lParam)->code == NM_DBLCLK) {
            HWND strip = GetDlgItem(hwnd, IDCX_TABSTRIP);
            bool apptab = (SendMessage(strip, TCM_GETCURSEL, 0, 0) == 1);
            kitty_cfg_goto_panel(apptab ? "Application/Workplace proxy"
                                        : "Session");
            return 0;
        }
        if (LOWORD(wParam) == IDCX_TABSTRIP &&
            ((LPNMHDR) lParam)->code == TCN_SELCHANGE) {
            /*
             * A tab change rebuilds the tree from the other half of the
             * ctrlbox and selects its first panel. Everything that makes a
             * panel change safe - hiding the old one's windows, swapping its
             * keyboard shortcuts out, creating or showing the new one - is
             * driven by the TVN_SELCHANGED that the selection below raises, so
             * this does not repeat any of it.
             *
             * The scroll offset is dropped first. It belongs to the panel that
             * was showing, and the tree is about to stop containing that panel
             * at all; kitty_cfg_panel_scrollbar would otherwise reset it
             * against whatever comes up next.
             */
            struct treeview_faff faff;
            HWND tv = GetDlgItem(hwnd, IDCX_TREEVIEW);
            HWND strip = GetDlgItem(hwnd, IDCX_TABSTRIP);
            bool apptab = (SendMessage(strip, TCM_GETCURSEL, 0, 0) == 1);
            const char *first;

            if (kitty_cfg_active_panel) {
                kitty_cfg_panel_scroll_to(hwnd, kitty_cfg_active_panel, 0);
                /* Remember where the user was on the tab being LEFT - this
                 * notification arrives after the tab has already changed, so
                 * that is the other one. */
                kitty_cfg_tab_last[apptab ? 0 : 1] =
                    kitty_cfg_active_panel->path;
                kitty_cfg_remember_app_panel(kitty_cfg_active_panel->path);
            }
            memset(&faff, 0, sizeof(faff));
            faff.treeview = tv;
            first = kitty_cfg_build_tree(pds, &faff, apptab);
            if (first) {
                /* Back to where they were on this tab, if they have been here
                 * before and that panel still exists; otherwise the top. */
                HTREEITEM want = NULL;
                HTREEITEM root = TreeView_GetRoot(tv);
                const char *last = kitty_cfg_tab_last[apptab ? 1 : 0];
                if (!last && apptab)
                    last = kitty_cfg_remembered_app_panel();
                if (last)
                    want = kitty_cfg_find_item(tv, root, last);
                if (!want)
                    want = root;
                if (want)
                    TreeView_SelectItem(tv, want);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCX_TREEVIEW &&
            ((LPNMHDR) lParam)->code == TVN_SELCHANGED) {
            /*
             * Selection-change events on the treeview cause us to do
             * a flurry of control deletion and creation - but only
             * after WM_INITDIALOG has finished. The initial
             * selection-change event(s) during treeview setup are
             * ignored.
             */
            HTREEITEM i;
            TVITEM item;
            char buffer[64];

            if (!pds->initialised)
                return 0;
            if (kitty_cfg_tree_rebuilding)
                return 0;              /* see the flag's definition */

            i = TreeView_GetSelection(((LPNMHDR) lParam)->hwndFrom);
            /*
             * NO SELECTION is a real event, not an impossible one: emptying
             * the tree raises this notification with the selection already
             * gone, which is what a tab change does before it refills the
             * tree. Without this the code below asked for an item that does
             * not exist, read the lParam that TreeView_GetItem therefore never
             * wrote, and took whatever was on the stack for a panel path -
             * hiding the visible panel and building a junk one for a garbage
             * pointer before the real selection arrived. That was visible as a
             * flicker on every tab switch, and it was one bad stack value away
             * from being a crash.
             */
            if (!i)
                return 0;

            item.hItem = i;
            item.pszText = buffer;
            item.cchTextMax = sizeof(buffer);
            item.mask = TVIF_TEXT | TVIF_PARAM;
            item.lParam = 0;          /* GetItem leaves it alone on failure */
            if (!TreeView_GetItem(((LPNMHDR) lParam)->hwndFrom, &item) ||
                !item.lParam)
                return 0;
            /*
             * KiTTY: swap the visible panel for the selected one - hide,
             * show or create; never destroy. See the panel cache above.
             */
            {
                const char *newpath = (const char *)item.lParam;
                struct kitty_cfg_panel *newpanel =
                    kitty_cfg_panel_find(newpath);

                if (newpanel && newpanel == kitty_cfg_active_panel) {
                    /* Re-selecting the visible panel: nothing to swap. */
                    SetFocus(((LPNMHDR) lParam)->hwndFrom);
                    return 0;
                }

                if (kitty_cfg_active_panel)
                    kitty_cfg_panel_show(pds->dp, kitty_cfg_active_panel,
                                         false);
                if (newpanel)
                    kitty_cfg_panel_show(pds->dp, newpanel, true);
                else
                    newpanel = kitty_cfg_panel_create(pds, newpath);
                kitty_cfg_active_panel = newpanel;
                /* Fit the scroll bar to whatever is showing now - or take it
                 * away, if this panel fits. */
                kitty_cfg_panel_scrollbar(hwnd, newpanel, false);

                /* The shown panel is refreshed every time it appears, and
                 * the button row keeps the refresh it always had. Panels
                 * that stay hidden are NOT refreshed here - a full
                 * dlg_refresh(NULL) would now reach every cached panel,
                 * which only a whole-Conf change (Load Session) needs. */
                kitty_cfg_panel_refresh(pds->dp, newpanel);
                kitty_cfg_winctrls_refresh(pds->dp,
                                           &pds->ctrltrees[TREE_BASE]);
            }

            /*
             * Repaint the PANEL, not the whole dialog.
             *
             * InvalidateRect(hwnd, NULL, true) erased and redrew everything -
             * the category tree, the buttons, the lot - on every panel switch,
             * even though only the right-hand side changed. Measured on
             * 2026-08-19: a quarter of the time spent switching panels was in
             * NtGdiPatBlt, i.e. erasing background that was about to be covered
             * by the same controls as before.
             *
             * The tree is on the left and does not change, so the region right
             * of it is what needs redrawing. If the tree's rectangle cannot be
             * had for any reason, fall back to the old behaviour rather than
             * leaving the dialog half-drawn.
             */
            {
                RECT client, tree;
                HWND treewin = ((LPNMHDR) lParam)->hwndFrom;
                if (GetClientRect(hwnd, &client) &&
                    GetWindowRect(treewin, &tree)) {
                    MapWindowPoints(NULL, hwnd, (LPPOINT)&tree, 2);
                    client.left = tree.right;
                    /*
                     * ONE pass, not two. InvalidateRect leaves the erase and the
                     * children's repaints to happen separately, which the eye
                     * sees as the panel appearing and then blinking once: the
                     * new controls draw themselves, and the deferred background
                     * erase then wipes and redraws underneath them.
                     * RDW_UPDATENOW | RDW_ALLCHILDREN does the erase and every
                     * child in a single synchronous pass before returning.
                     */
                    RedrawWindow(hwnd, &client, NULL,
                                 RDW_ERASE | RDW_INVALIDATE |
                                 RDW_UPDATENOW | RDW_ALLCHILDREN);
                    /*
                     * The TREE still has to be repainted, without the erase.
                     *
                     * Its selection has just moved while the panel swap
                     * deferred painting, so the invalidation the control would
                     * normally do for itself was swallowed. Leaving it out left
                     * the old row highlighted as well as the new one - two
                     * selected-looking rows at once. It needs no background
                     * erase, which is where the cost was.
                     */
                    InvalidateRect(treewin, NULL, false);
                } else {
                    InvalidateRect(hwnd, NULL, true);
                }
            }

            SetFocus(((LPNMHDR) lParam)->hwndFrom);     /* ensure focus stays */
        }
        return 0;

      default:
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
    }
}

void modal_about_box(HWND hwnd)
{
    /* KiTTY: no longer modal - a non-blocking About placed over the caller. */
    kitty_show_about_modeless(hwnd);
}

void show_help(HWND hwnd)
{
    launch_help(hwnd, NULL);
}

void defuse_showwindow(void)
{
    /*
     * Work around the fact that the app's first call to ShowWindow
     * will ignore the default in favour of the shell-provided
     * setting.
     */
    {
        HWND hwnd;
        hwnd = CreateDialog(hinst, MAKEINTRESOURCE(IDD_ABOUTBOX),
                            NULL, NullDlgProc);
        ShowWindow(hwnd, SW_HIDE);
        SetActiveWindow(hwnd);
        DestroyWindow(hwnd);
    }
}

/* KiTTY: say in the configuration box's title bar when quick connect is on -
 * the mode where the box starts from the defaults and a host is typed rather
 * than picked. Until this, the only sign of it was where the caret happened to
 * be, so the mode could be on or off with nothing on screen to say which.
 *
 * It retitles the LIVE window rather than dp->wintitle, because the mode is
 * armed and disarmed while the box is open (loading "Default Settings" arms it,
 * loading anything else disarms it), and dp->wintitle is the unmarked base to
 * go back to.
 *
 * Not inside #ifdef MOD_PERSO: this file compiles into the shared GUI library
 * without that define, and kitty_config.c - which does have it - is the caller.
 * Harmless in the stock variants, which never call it. */
void kitty_dlg_mark_quickconnect(dlgparam *dp, int on)
{
    if (!dp || !dp->hwnd || !dp->wintitle)
        return;
    if (on) {
        /* In FRONT of the test-build stamp, not after it. Appended, the marker
         * was the first thing to fall off the end of a title bar that already
         * carried "(portable)" and a build label - which is exactly the window
         * where it was wanted. */
        const char *stamp = strstr(dp->wintitle, "  *** ");
        char *t;
        if (stamp)
            t = dupprintf("%.*s - quick connect%s",
                          (int)(stamp - dp->wintitle), dp->wintitle, stamp);
        else
            t = dupprintf("%s - quick connect", dp->wintitle);
        SetWindowText(dp->hwnd, t);
        sfree(t);
    } else {
        SetWindowText(dp->hwnd, dp->wintitle);
    }
}

bool do_config(Conf *conf)
{
    bool ret;
    PortableDialogStuff *pds = pds_new(2);

    setup_config_box(pds->ctrlbox, false, 0, 0);
    win_setup_config_box(pds->ctrlbox, &pds->dp->hwnd, has_help(), false, 0);

    /* Tag the title in portable (file-storage) mode so the user can tell a
     * portable KiTTY from an installed one at a glance. */
    {
        extern int kitty_storage_is_portable(void);
        extern char *kitty_title_compose(const char *, int, int, int);
        /* ...and with the restricted process ACL, since this window is what a
         * "kitty.exe -restrict-acl" with no session shows: the terminal title
         * carries the same marker, but there is no terminal yet here.
         * Suffixes composed in kitty/kitty_title.c - one place for all. */
        char *base = dupprintf("%s Configuration", appname);
        if (dialog_box_demo_screenshot_filename) {
            /* Demo-screenshot mode exists only to render the box for the
             * documentation, so the title must be the CANONICAL one - no
             * (portable), no ACL marker, no test-build label. The clean
             * room the screenshot pipeline runs in is portable as a
             * mechanism, not as part of the story the picture tells. */
            pds->dp->wintitle = base;
        } else {
            pds->dp->wintitle = kitty_title_compose(
                base, kitty_storage_is_portable(), restricted_acl(), true);
            sfree(base);
        }
    }
    pds->dp->data = conf;

    dlg_auto_set_fixed_pitch_flag(pds->dp);

    pds->dp->shortcuts['g'] = true;          /* the treeview: `Cate&gory' */

    ret = ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_MAINBOX), "PuTTYConfigBox",
                         NULL, GenericMainDlgProc, pds);

    pds_free(pds);

    return ret;
}

bool do_reconfig(HWND hwnd, Conf *conf, int protcfginfo)
{
    Conf *backup_conf;
    bool ret;
    int protocol;
    PortableDialogStuff *pds = pds_new(2);

    backup_conf = conf_copy(conf);

    protocol = conf_get_int(conf, CONF_protocol);
    setup_config_box(pds->ctrlbox, true, protocol, protcfginfo);
    win_setup_config_box(pds->ctrlbox, &pds->dp->hwnd, has_help(),
                         true, protocol);

    {
        extern int kitty_storage_is_portable(void);
        extern char *kitty_title_compose(const char *, int, int, int);
        /* This window missed (RESTRICTED) while every title rolled its own
         * suffixes - composed in kitty/kitty_title.c now, one place for all. */
        char *base = dupprintf("%s Reconfiguration", appname);
        pds->dp->wintitle = kitty_title_compose(
            base, kitty_storage_is_portable(), restricted_acl(), true);
        sfree(base);
    }
    pds->dp->data = conf;

    dlg_auto_set_fixed_pitch_flag(pds->dp);

    pds->dp->shortcuts['g'] = true;          /* the treeview: `Cate&gory' */

    ret = ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_MAINBOX), "PuTTYConfigBox",
                         NULL, GenericMainDlgProc, pds);

    pds_free(pds);

    if (!ret)
        conf_copy_into(conf, backup_conf);

    conf_free(backup_conf);

    return ret;
}

static void win_gui_eventlog(LogPolicy *lp, const char *string)
{
    char timebuf[40];
    char **location;
    struct tm tm;

    tm=ltime();
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S\t", &tm);

#ifdef MOD_NETDEBUG
    /* KiTTY network-debug build: tee every event-log line - which carries the
     * whole connection lifecycle AND the disconnect REASON ("Network error: ...",
     * "Server unexpectedly closed...", keepalives, reconnect attempts) - to
     * %USERPROFILE%\kitty_netdebug.log with millisecond timestamps, so an
     * intermittent disconnect can be captured and analysed offline later. */
    {
        static FILE *ndf = NULL;
        SYSTEMTIME s; GetLocalTime(&s);
        if (!ndf) {
            char p[MAX_PATH]; const char *h = getenv("USERPROFILE");
            snprintf(p, sizeof(p), "%s\\kitty_netdebug.log", h ? h : "C:");
            ndf = fopen(p, "a");
            if (ndf)
                fprintf(ndf, "\n===== kitty_NETDEBUG start %04d-%02d-%02d %02d:%02d:%02d =====\n",
                        s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
        }
        if (ndf) {
            fprintf(ndf, "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\n",
                    s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond,
                    s.wMilliseconds, string);
            fflush(ndf);
        }
    }
#endif

    if (ninitial < LOGEVENT_INITIAL_MAX)
        location = &events_initial[ninitial];
    else
        location = &events_circular[(circular_first + ncircular) % LOGEVENT_CIRCULAR_MAX];

    if (*location)
        sfree(*location);
    *location = dupcat(timebuf, string);
    if (logbox) {
        int count;
        SendDlgItemMessage(logbox, IDN_LIST, LB_ADDSTRING,
                           0, (LPARAM) *location);
        count = SendDlgItemMessage(logbox, IDN_LIST, LB_GETCOUNT, 0, 0);
        SendDlgItemMessage(logbox, IDN_LIST, LB_SETTOPINDEX, count - 1, 0);

        update_logbox_horizontal_extent(logbox);
    }
    if (ninitial < LOGEVENT_INITIAL_MAX) {
        ninitial++;
    } else if (ncircular < LOGEVENT_CIRCULAR_MAX) {
        ncircular++;
    } else if (ncircular == LOGEVENT_CIRCULAR_MAX) {
        circular_first = (circular_first + 1) % LOGEVENT_CIRCULAR_MAX;
        sfree(events_circular[circular_first]);
        events_circular[circular_first] = dupstr("..");
    }
}

static void win_gui_logging_error(LogPolicy *lp, const char *event)
{
    WinGuiSeat *wgs = container_of(lp, WinGuiSeat, logpolicy);

    /* Send 'can't open log file' errors to the terminal window.
     * (Marked as stderr, although terminal.c won't care.) */
    seat_stderr_pl(&wgs->seat, ptrlen_from_asciz(event));
    seat_stderr_pl(&wgs->seat, PTRLEN_LITERAL("\r\n"));
}

void showeventlog(HWND hwnd)
{
    if (!logbox) {
        logbox = CreateDialog(hinst, MAKEINTRESOURCE(IDD_LOGBOX),
                              hwnd, LogProc);
        ShowWindow(logbox, SW_SHOWNORMAL);
    }
    SetActiveWindow(logbox);
}

/* KiTTY: one non-modal About window, placed over the owner (kitty_auxpos) and
 * reused if already open. Registered as an aux dialog so the active message
 * loop keeps its keyboard handling (Esc closes, Tab cycles) working. */
static void kitty_show_about_modeless(HWND owner)
{
    if (kitty_about_dlg && IsWindow(kitty_about_dlg)) {
        SetForegroundWindow(kitty_about_dlg);
        return;
    }
    kitty_about_dlg = CreateDialog(hinst, MAKEINTRESOURCE(IDD_ABOUTBOX), owner, AboutProc);
    if (kitty_about_dlg) {
        ShinyAddAuxDialog(kitty_about_dlg);
        ShowWindow(kitty_about_dlg, SW_SHOW);
        SetForegroundWindow(kitty_about_dlg);
    }
}
void showabout(HWND hwnd)
{
    kitty_show_about_modeless(hwnd);
}

struct hostkey_dialog_ctx {
    SeatDialogText *text;
    bool has_title;
    const char *helpctx;
};

static INT_PTR HostKeyMoreInfoProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam, void *vctx)
{
    struct hostkey_dialog_ctx *ctx = (struct hostkey_dialog_ctx *)vctx;

    switch (msg) {
      case WM_INITDIALOG: {
        int index = 100, y = 12;

        WPARAM font = SendMessage(hwnd, WM_GETFONT, 0, 0);

        const char *key = NULL;
        for (SeatDialogTextItem *item = ctx->text->items,
                 *end = item + ctx->text->nitems; item < end; item++) {
            switch (item->type) {
              case SDT_MORE_INFO_KEY:
                key = item->text;
                break;
              case SDT_MORE_INFO_VALUE_SHORT:
              case SDT_MORE_INFO_VALUE_BLOB: {
                RECT rk, rv;
                DWORD editstyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    ES_AUTOHSCROLL | ES_READONLY;
                if (item->type == SDT_MORE_INFO_VALUE_BLOB) {
                    rk.left = 12;
                    rk.right = 376;
                    rk.top = y;
                    rk.bottom = 8;
                    y += 10;

                    editstyle |= ES_MULTILINE;
                    rv.left = 12;
                    rv.right = 376;
                    rv.top = y;
                    rv.bottom = 64;
                    y += 68;
                } else {
                    rk.left = 12;
                    rk.right = 80;
                    rk.top = y+2;
                    rk.bottom = 8;

                    rv.left = 100;
                    rv.right = 288;
                    rv.top = y;
                    rv.bottom = 12;

                    y += 16;
                }

                MapDialogRect(hwnd, &rk);
                HWND ctl = CreateWindowEx(
                    0, "STATIC", key, WS_CHILD | WS_VISIBLE,
                    rk.left, rk.top, rk.right, rk.bottom,
                    hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));

                MapDialogRect(hwnd, &rv);
                ctl = CreateWindowEx(
                    WS_EX_CLIENTEDGE, "EDIT", item->text, editstyle,
                    rv.left, rv.top, rv.right, rv.bottom,
                    hwnd, (HMENU)(ULONG_PTR)index++, hinst, NULL);
                SendMessage(ctl, WM_SETFONT, font, MAKELPARAM(true, 0));
                break;
              }
              default:
                break;
            }
        }

        /*
         * Now resize the overall window, and move the Close button at
         * the bottom.
         */
        RECT r;
        r.left = 176;
        r.top = y + 10;
        r.right = r.bottom = 0;
        MapDialogRect(hwnd, &r);
        HWND ctl = GetDlgItem(hwnd, IDOK);
        SetWindowPos(ctl, NULL, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOREDRAW | SWP_NOZORDER);

        r.left = r.top = r.right = 0;
        r.bottom = 300;
        MapDialogRect(hwnd, &r);
        int oldheight = r.bottom;

        r.left = r.top = r.right = 0;
        r.bottom = y + 30;
        MapDialogRect(hwnd, &r);
        int newheight = r.bottom;

        GetWindowRect(hwnd, &r);

        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left,
                     r.bottom - r.top + newheight - oldheight,
                     SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

        ShowWindow(hwnd, SW_SHOWNORMAL);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            ShinyEndDialog(hwnd, 0);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        ShinyEndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

static const char *process_seatdialogtext(
    strbuf *dlg_text, const char **scary_heading, SeatDialogText *text)
{
    const char *dlg_title = "";

    for (SeatDialogTextItem *item = text->items,
             *end = item + text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_PARA:
            put_fmt(dlg_text, "%s\r\n\r\n", item->text);
            break;
          case SDT_DISPLAY:
            put_fmt(dlg_text, "%s\r\n\r\n", item->text);
            break;
          case SDT_SCARY_HEADING:
            assert(scary_heading != NULL && "only expect a scary heading if "
                   "the dialog has somewhere to put it");
            *scary_heading = item->text;
            break;
          case SDT_TITLE:
            dlg_title = item->text;
            break;
          default:
            break;
        }
    }

    /* Trim any trailing newlines */
    while (strbuf_chomp(dlg_text, '\r') || strbuf_chomp(dlg_text, '\n'));

    return dlg_title;
}

static INT_PTR HostKeyDialogProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam, void *vctx)
{
    struct hostkey_dialog_ctx *ctx = (struct hostkey_dialog_ctx *)vctx;

    switch (msg) {
      case WM_INITDIALOG: {
        strbuf *dlg_text = strbuf_new();
        const char *scary_heading = NULL;
        const char *dlg_title = process_seatdialogtext(
            dlg_text, &scary_heading, ctx->text);

        LPCTSTR iconid = IDI_QUESTION;
        if (scary_heading) {
            SetDlgItemText(hwnd, IDC_HK_TITLE, scary_heading);
            iconid = IDI_WARNING;
        }

        SetDlgItemText(hwnd, IDC_HK_TEXT, dlg_text->s);
        MakeDlgItemBorderless(hwnd, IDC_HK_TEXT);
        strbuf_free(dlg_text);

        SetWindowText(hwnd, dlg_title);

        if (!ctx->has_title) {
            HWND item = GetDlgItem(hwnd, IDC_HK_TITLE);
            if (item)
                DestroyWindow(item);
        }

        /*
         * Find out how tall the text in the edit control really ended
         * up (after line wrapping), and adjust the height of the
         * whole box to match it.
         */
        int height = SendDlgItemMessage(hwnd, IDC_HK_TEXT,
                                        EM_GETLINECOUNT, 0, 0);
        height *= 8; /* height of a text line, by definition of dialog units */

        int edittop = ctx->has_title ? 40 : 20;

        RECT r;
        r.left = 40;
        r.top = edittop;
        r.right = 290;
        r.bottom = height;
        MapDialogRect(hwnd, &r);
        SetWindowPos(GetDlgItem(hwnd, IDC_HK_TEXT), NULL,
                     r.left, r.top, r.right, r.bottom,
                     SWP_NOREDRAW | SWP_NOZORDER);

        static const struct {
            int id, x;
        } buttons[] = {
            { IDCANCEL, 288 },
            { IDC_HK_ACCEPT, 168 },
            { IDC_HK_ONCE, 216 },
            { IDC_HK_MOREINFO, 60 },
            { IDHELP, 12 },
        };
        for (size_t i = 0; i < lenof(buttons); i++) {
            HWND ctl = GetDlgItem(hwnd, buttons[i].id);
            r.left = buttons[i].x;
            r.top = edittop + height + 20;
            r.right = r.bottom = 0;
            MapDialogRect(hwnd, &r);
            SetWindowPos(ctl, NULL, r.left, r.top, 0, 0,
                         SWP_NOSIZE | SWP_NOREDRAW | SWP_NOZORDER);
        }

        r.left = r.top = r.right = 0;
        r.bottom = 240;
        MapDialogRect(hwnd, &r);
        int oldheight = r.bottom;

        r.left = r.top = r.right = 0;
        r.bottom = edittop + height + 40;
        MapDialogRect(hwnd, &r);
        int newheight = r.bottom;

        GetWindowRect(hwnd, &r);

        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left,
                     r.bottom - r.top + newheight - oldheight,
                     SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

        HANDLE icon = LoadImage(
            NULL, iconid, IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_SHARED);
        SendDlgItemMessage(hwnd, IDC_HK_ICON, STM_SETICON, (WPARAM)icon, 0);

        if (!has_help()) {
            HWND item = GetDlgItem(hwnd, IDHELP);
            if (item)
                DestroyWindow(item);
        }

        ShowWindow(hwnd, SW_SHOWNORMAL);

        return 1;
      }
      case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND control = (HWND)lParam;

        if (GetWindowLongPtr(control, GWLP_ID) == IDC_HK_TITLE &&
            ctx->has_title) {
            SetBkMode(hdc, TRANSPARENT);
            HFONT prev_font = (HFONT)SelectObject(
                hdc, (HFONT)GetStockObject(SYSTEM_FONT));
            LOGFONT lf;
            if (GetObject(prev_font, sizeof(lf), &lf)) { 
                lf.lfWeight = FW_BOLD;
                lf.lfHeight = lf.lfHeight * 3 / 2;
                HFONT bold_font = CreateFontIndirect(&lf);
                if (bold_font)
                    SelectObject(hdc, bold_font);
            }
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_HK_ACCEPT:
          case IDC_HK_ONCE:
          case IDCANCEL:
            ShinyEndDialog(hwnd, LOWORD(wParam));
            return 0;
          case IDHELP: {
            launch_help(hwnd, ctx->helpctx);
            return 0;
          }
          case IDC_HK_MOREINFO: {
            ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_HK_MOREINFO),
                           "PuTTYHostKeyMoreInfo", hwnd,
                           HostKeyMoreInfoProc, ctx);
          }
        }
        return 0;
      case WM_CLOSE:
        ShinyEndDialog(hwnd, IDCANCEL);
        return 0;
    }
    return 0;
}

const SeatDialogPromptDescriptions *win_seat_prompt_descriptions(Seat *seat)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "press \"Accept\"",
        .hk_connect_once_action = "press \"Connect Once\"",
        .hk_cancel_action = "press \"Cancel\"",
        .hk_cancel_action_Participle = "Pressing \"Cancel\"",
        .weak_accept_action = "press \"Yes\"",
        .weak_cancel_action = "press \"No\"",
    };
    return &descs;
}

/* ----------------------------------------------------------------------
 * KiTTY: inline-first security prompts (successor to cyd01/KiTTY #548).
 *
 * By default the host-key and weak-crypto confirmations use the classic modal
 * dialog boxes below. When the matching kitty.ini [KiTTY] flag is set to no,
 * we surface the confirmation OpenSSH-style instead: the factual details
 * (host, fingerprint, warning) are written into the terminal and the user
 * types "yes" to accept. A changed host key in inline mode is a hard abort
 * (like OpenSSH: the cached key must be cleared explicitly first). During a
 * rekey - i.e. the session has already authenticated, so the running program
 * owns the input line - inline prompting is impossible, so inline mode aborts
 * with an in-terminal error (OpenSSH does the same).
 */

extern int GetModalNewHostKeyConfirmationFlag(void);
extern int GetModalChangedHostKeyConfirmationFlag(void);
extern int GetModalWeakKeyConfirmationFlag(void);

/*
 * Master gate for the inline (in-terminal, OpenSSH-style) security prompts
 * (item 4). Previously forced to 0 because the inline path crashed: writing to
 * the terminal during the initial-connect host-key confirmation faulted. That
 * was NOT a terminal-lifecycle problem - it was a WinGuiSeat struct-layout
 * mismatch. dialog.c (built into the guiterminal static lib, no MOD_RECONNECT)
 * and window.c (built into the kitty exe target, MOD_RECONNECT defined) saw
 * `term` at different offsets, so dialog.c read wgs->term from the wrong offset
 * and wrote to a bogus terminal. Fixed by making the MOD_RECONNECT-guarded
 * WinGuiSeat fields unconditional (see win-gui-seat.h), so the layout is
 * identical in every TU. Now safe to enable.
 * (Deliberately non-const so the code stays referenced under all build configs.)
 */
static int kitty_inline_prompts_enabled = 1;

static void kitty_term_puts(Terminal *term, const char *s)
{
    if (s && *s)
        term_data(term, s, strlen(s));
}

/* Wrap KiTTY's inline security text to a fixed, comfortable column width so it
 * reads the same regardless of the (possibly very wide or narrow) terminal. */
#define KITTY_WRAP_COLS 76

/* Visible width of the first n bytes of s, ignoring ANSI CSI escape sequences
 * (e.g. the bold-red highlight) so they do not count toward the column. */
static size_t kitty_visible_width(const char *s, size_t n)
{
    size_t w = 0, i = 0;
    while (i < n) {
        if (s[i] == '\033' && i + 1 < n && s[i+1] == '[') {
            i += 2;
            while (i < n && !(s[i] >= '@' && s[i] <= '~'))
                i++;
            if (i < n)
                i++;               /* consume the final byte of the sequence */
        } else {
            w++;
            i++;
        }
    }
    return w;
}

/* Word-wrap s to KITTY_WRAP_COLS visible columns, honouring explicit newlines
 * (hard breaks) and collapsing runs of whitespace. ANSI colour escapes are
 * copied through but not counted. A single trailing space is preserved (so a
 * prompt keeps its gap before the typed answer). Returns a fresh CRLF-delimited
 * string owned by the caller. */
static char *kitty_wrap_dup(const char *s)
{
    strbuf *sb = strbuf_new();
    size_t col = 0;
    const char *p = s;
    while (*p) {
        if (*p == '\n') {
            put_data(sb, "\r\n", 2);
            col = 0;
            p++;
            continue;
        }
        if (*p == '\r' || *p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        const char *w = p;
        while (*w && *w != ' ' && *w != '\t' && *w != '\n' && *w != '\r')
            w++;
        size_t vw = kitty_visible_width(p, (size_t)(w - p));
        if (col > 0 && col + 1 + vw > KITTY_WRAP_COLS) {
            put_data(sb, "\r\n", 2);
            col = 0;
        }
        if (col > 0) {
            put_byte(sb, ' ');
            col++;
        }
        put_data(sb, p, (size_t)(w - p));
        col += vw;
        p = w;
    }
    size_t len = strlen(s);
    if (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'))
        put_byte(sb, ' ');
    return strbuf_to_str(sb);
}

/* Wrap s and write it straight to the terminal (intro / warning prose). */
static void kitty_term_wrapped(Terminal *term, const char *s)
{
    char *w = kitty_wrap_dup(s);
    kitty_term_puts(term, w);
    sfree(w);
}

/*
 * Append the factual portion of a SeatDialogText to a strbuf: the scary
 * heading (if any), the descriptive paragraphs and the displayed values (host,
 * fingerprint), stopping at the SDT_BATCH_ABORT divider. Everything after that
 * divider is PuTTY-button-specific guidance ("press Accept"), which does not
 * apply to a typed prompt, so we supply our own wording instead. We capture
 * into a strbuf rather than writing straight to the terminal because this runs
 * synchronously inside the SSH kex coroutine, where touching the terminal
 * (term_data -> term_out) faults; the text is emitted later from a top-level
 * callback (kitty_inline_dispatch).
 */
static void kitty_facts_to_strbuf(strbuf *sb, SeatDialogText *text)
{
    for (SeatDialogTextItem *item = text->items,
             *end = item + text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_SCARY_HEADING:
          case SDT_PARA:
            if (item->text) {
                char *w = kitty_wrap_dup(item->text);
                put_fmt(sb, "%s\r\n", w);
                sfree(w);
            }
            break;
          case SDT_DISPLAY:
            put_fmt(sb, "  %s\r\n", item->text ? item->text : "");
            break;
          case SDT_BATCH_ABORT:
            /* Divider: the remaining items are button-specific guidance. */
            return;
          default:
            break;
        }
    }
}

/* Does the user's typed response equal `want` (case-insensitive, surrounding
 * whitespace ignored)? `want` must be given in lower case. */
static bool kitty_response_is_word(const char *s, const char *want)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    for (; *want; want++, s++)
        if (tolower((unsigned char)*s) != *want)
            return false;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == '\0';
}

/* Is the user's typed response an affirmative "yes" (OpenSSH style)? */
static bool kitty_response_is_yes(const char *s)
{
    return kitty_response_is_word(s, "yes");
}

struct kitty_inline_confirm_ctx {
    Seat *seat;
    Terminal *term;
    strbuf *facts;                     /* captured factual text to emit */
    char *promptline;                  /* dup'd; non-NULL => prompt for "yes" */
    char *abortmsg;                    /* dup'd; used when promptline == NULL */
    prompts_t *prompts;                /* built by the deferred setup (prompt mode) */
    bool store_on_yes;                 /* accept => store_host_key (new key only) */
    char *host, *keytype, *keystr;     /* dup'd; for store_host_key on accept */
    int port;
    void (*ssh_callback)(void *ctx, SeatPromptResult result);
    void *ssh_cbctx;
    int changed_step;                  /* changed-key double opt-in: 0=n/a, 1..3 */
};

static void kitty_inline_confirm_free(struct kitty_inline_confirm_ctx *c)
{
    if (c->prompts)
        free_prompts(c->prompts);
    if (c->facts)
        strbuf_free(c->facts);
    sfree(c->promptline);
    sfree(c->abortmsg);
    sfree(c->host);
    sfree(c->keytype);
    sfree(c->keystr);
    sfree(c);
}

/*
 * Toplevel callback fired when the inline "yes/no" line has been entered (or
 * the prompt aborted, e.g. via ^C or a missing ldisc). Interprets the response
 * and delivers the result to the SSH layer's confirmation callback.
 */
static void kitty_inline_confirm_done(void *vctx)
{
    struct kitty_inline_confirm_ctx *c = (struct kitty_inline_confirm_ctx *)vctx;
    prompts_t *p = c->prompts;
    SeatPromptResult spr;

    const char *resp = (p->spr.kind == SPRK_OK) ?
        prompt_get_result_ref(p->prompts[0]) : NULL;

    if (resp && kitty_response_is_yes(resp)) {
        if (c->store_on_yes)
            store_host_key(c->seat, c->host, c->port, c->keytype, c->keystr);
        spr = SPR_OK;
    } else if (resp && c->store_on_yes && kitty_response_is_word(resp, "once")) {
        /* New host key only: connect this once without caching it (parity with
         * the modal "Connect Once"). */
        kitty_term_puts(c->term, "Connecting once; the key was not cached.\r\n");
        spr = SPR_OK;
    } else {
        kitty_term_puts(c->term, "Connection abandoned.\r\n");
        spr = SPR_USER_ABORT;
    }

    void (*cb)(void *, SeatPromptResult) = c->ssh_callback;
    void *cbctx = c->ssh_cbctx;
    kitty_inline_confirm_free(c);
    cb(cbctx, spr);
}

/*
 * Top-level callback that actually touches the terminal. It is scheduled by
 * kitty_inline_confirm so that all terminal output happens OUTSIDE the SSH kex
 * coroutine's call stack (writing to the terminal from inside that stack, via
 * term_data -> term_out, faults). It prints the captured factual text, then
 * either aborts with a message or arms the inline "type yes" prompt.
 */
static void kitty_inline_dispatch(void *vctx)
{
    struct kitty_inline_confirm_ctx *c = (struct kitty_inline_confirm_ctx *)vctx;

    if (c->facts && c->facts->len)
        term_data(c->term, c->facts->s, c->facts->len);
    strbuf_free(c->facts);
    c->facts = NULL;

    if (!c->promptline) {
        /* Abort-only path (rekey / changed-key hard abort). */
        kitty_term_wrapped(c->term, c->abortmsg);
        void (*cb)(void *, SeatPromptResult) = c->ssh_callback;
        void *cbctx = c->ssh_cbctx;
        kitty_inline_confirm_free(c);
        cb(cbctx, SPR_USER_ABORT);
        return;
    }

    /* Prompt path: arm the terminal line editor for a typed "yes". Once armed,
     * the ldisc drives input to completion and queues kitty_inline_confirm_done
     * (even on the no-ldisc error path), so we always resolve there. */
    prompts_t *p = new_prompts();
    p->to_server = false;
    p->from_server = false;
    p->name = dupstr("SSH security confirmation");
    p->name_reqd = false;
    p->callback = kitty_inline_confirm_done;
    p->callback_ctx = c;
    p->utf8 = true;
    add_prompt(p, kitty_wrap_dup(c->promptline), true /* echo the typed answer */);
    c->prompts = p;

    term_get_userpass_input(c->term, p);
}

/*
 * Entry point for every inline (non-modal) security confirmation. Called
 * synchronously from inside the SSH coroutine, so it must NOT touch the
 * terminal: it captures the factual text into a strbuf and defers all terminal
 * work to kitty_inline_dispatch via a top-level callback. Always returns
 * SPR_INCOMPLETE; the result is delivered later through the SSH callback.
 *
 *   promptline != NULL -> prompt for a typed "yes" (accept) vs anything else.
 *   promptline == NULL -> abort-only: print abortmsg, then SPR_USER_ABORT.
 */
static SeatPromptResult kitty_inline_confirm(
    WinGuiSeat *wgs, SeatDialogText *text, const char *promptline,
    const char *abortmsg, bool store_on_yes, const char *host, int port,
    const char *keytype, const char *keystr,
    void (*callback)(void *ctx, SeatPromptResult result), void *cbctx)
{
    struct kitty_inline_confirm_ctx *c = snew(struct kitty_inline_confirm_ctx);
    c->seat = &wgs->seat;
    c->term = wgs->term;
    c->facts = strbuf_new();
    kitty_facts_to_strbuf(c->facts, text);   /* string build only - no term I/O */
    c->promptline = promptline ? dupstr(promptline) : NULL;
    c->abortmsg = abortmsg ? dupstr(abortmsg) : NULL;
    c->prompts = NULL;
    c->store_on_yes = store_on_yes;
    c->host = host ? dupstr(host) : NULL;
    c->keytype = keytype ? dupstr(keytype) : NULL;
    c->keystr = keystr ? dupstr(keystr) : NULL;
    c->port = port;
    c->ssh_callback = callback;
    c->ssh_cbctx = cbctx;
    c->changed_step = 0;

    queue_toplevel_callback(kitty_inline_dispatch, c);
    return SPR_INCOMPLETE;
}

/* True if the session has already authenticated, so a host-key/weak-crypto
 * confirmation now is a rekey and the terminal is owned by the running
 * program: we cannot present an inline prompt. */
static bool kitty_session_is_live(WinGuiSeat *wgs)
{
    /* ever_authenticated is now an unconditional WinGuiSeat field (see
     * win-gui-seat.h). dialog.c is built without MOD_RECONNECT (guiterminal lib),
     * so the old #ifdef here always returned false; read the field directly. */
    return wgs->ever_authenticated;
}

/* ----------------------------------------------------------------------
 * Changed host key: inline "double opt-in" (branch A).
 *
 * A *changed* key (one different from the one cached) is more suspicious than a
 * brand-new key, so it is confirmed in two conscious steps:
 *   1. Accept the new key for THIS connection ("yes"), or abandon.
 *   2. Optionally REPLACE the stored key for future connections. This second
 *      step demands the exact word "confirmed"; a reflexive "yes" is caught and
 *      re-asked (step 3) rather than accepted or silently cancelled. Declining
 *      leaves the old key cached and connects once.
 * The step machine reuses the terminal userpass prompt, re-arming the next
 * prompt from a fresh top-level callback so we never re-enter
 * term_get_userpass_input from inside its own completion callback.
 * ---------------------------------------------------------------------- */

/* ANSI SGR: bold red for the security-critical words, reset afterwards, so the
 * eye is drawn to CHANGED / SECURITY WARNING even in a wall of prompt text. */
#define KCH_HL  "\033[1;31m"
#define KCH_RST "\033[0m"

static const char KCH_ACK_INTRO[] =
    "\nThe host key for this server has " KCH_HL "CHANGED" KCH_RST " since it was "
    "last cached. This can mean the server was legitimately rebuilt - or that the "
    "connection is being intercepted (a man-in-the-middle attack).\n";
static const char KCH_ACK_PROMPT[] =
    "Type \"yes\" to accept the new key for THIS connection, or anything else "
    "to abandon: ";
static const char KCH_REPLACE_INTRO[] =
    "\nReplace the stored host key with this new one for future connections?\n"
    KCH_HL "SECURITY WARNING" KCH_RST ": KiTTY cannot confirm that this new key "
    "genuinely belongs to the server. A plain SSH host key is trusted on first "
    "use, with no authority to verify it against. Replace the stored key ONLY if "
    "you are certain, by some independent means (e.g. a fingerprint obtained "
    "out-of-band), that the new key is genuine.\n";
static const char KCH_REPLACE_PROMPT[] =
    "Type \"confirmed\" to replace the stored key, \"no\" or Enter to keep the "
    "old key, or Ctrl-C to abandon: ";
static const char KCH_RETRY_INTRO[] =
    "\nPlease answer with a whole word: \"confirmed\" to replace the stored key, "
    "or \"no\" (or Enter) to keep the old key and connect once. A plain \"yes\" "
    "is intentionally not enough to replace a changed key.\n";

static void kitty_inline_changed_done(void *vctx);   /* fwd */

/* Deliver the SSH result and tear down the context. */
static void kitty_inline_finish(struct kitty_inline_confirm_ctx *c,
                                SeatPromptResult spr)
{
    void (*cb)(void *, SeatPromptResult) = c->ssh_callback;
    void *cbctx = c->ssh_cbctx;
    kitty_inline_confirm_free(c);
    cb(cbctx, spr);
}

/* Arm one inline prompt line (its explanatory text is written separately). */
static void kitty_changed_arm_prompt(struct kitty_inline_confirm_ctx *c,
                                     const char *promptline)
{
    prompts_t *p = new_prompts();
    p->to_server = false;
    p->from_server = false;
    p->name = dupstr("SSH host key changed");
    p->name_reqd = false;
    p->callback = kitty_inline_changed_done;
    p->callback_ctx = c;
    p->utf8 = true;
    add_prompt(p, kitty_wrap_dup(promptline), true /* echo the typed answer */);
    c->prompts = p;
    term_get_userpass_input(c->term, p);
}

/* Top-level callback: emit the current step's text and arm its prompt. */
static void kitty_inline_changed_arm(void *vctx)
{
    struct kitty_inline_confirm_ctx *c = (struct kitty_inline_confirm_ctx *)vctx;
    switch (c->changed_step) {
      case 1:
        kitty_term_wrapped(c->term, KCH_ACK_INTRO);
        kitty_changed_arm_prompt(c, KCH_ACK_PROMPT);
        break;
      case 2:
        kitty_term_wrapped(c->term, KCH_REPLACE_INTRO);
        kitty_changed_arm_prompt(c, KCH_REPLACE_PROMPT);
        break;
      case 3:
        kitty_term_wrapped(c->term, KCH_RETRY_INTRO);
        kitty_changed_arm_prompt(c, KCH_REPLACE_PROMPT);
        break;
    }
}

/* First top-level callback: emit the factual details, then start step 1. */
static void kitty_inline_changed_dispatch(void *vctx)
{
    struct kitty_inline_confirm_ctx *c = (struct kitty_inline_confirm_ctx *)vctx;
    if (c->facts && c->facts->len)
        term_data(c->term, c->facts->s, c->facts->len);
    strbuf_free(c->facts);
    c->facts = NULL;
    c->changed_step = 1;
    kitty_inline_changed_arm(c);
}

static void kitty_changed_store_and_connect(struct kitty_inline_confirm_ctx *c)
{
    store_host_key(c->seat, c->host, c->port, c->keytype, c->keystr);
    kitty_term_puts(c->term, "\r\nStored host key replaced. Continuing.\r\n");
    kitty_inline_finish(c, SPR_OK);
}

static void kitty_changed_connect_once(struct kitty_inline_confirm_ctx *c)
{
    kitty_term_wrapped(c->term,
                    "\nKeeping the previously stored key; continuing this once "
                    "(you will be asked again next time).\n");
    kitty_inline_finish(c, SPR_OK);
}

/* Prompt-completion callback for every step of the changed-key flow. */
static void kitty_inline_changed_done(void *vctx)
{
    struct kitty_inline_confirm_ctx *c = (struct kitty_inline_confirm_ctx *)vctx;
    prompts_t *p = c->prompts;
    bool ok = (p->spr.kind == SPRK_OK);
    const char *resp = ok ? prompt_get_result_ref(p->prompts[0]) : NULL;
    bool is_yes = ok && kitty_response_is_yes(resp);
    bool is_confirmed = ok && kitty_response_is_word(resp, "confirmed");
    bool is_no = ok && kitty_response_is_word(resp, "no");
    bool is_empty = ok && kitty_response_is_word(resp, "");  /* bare Enter */

    switch (c->changed_step) {
      case 1:
        /* Acknowledge the change (gate to connect at all). A non-"yes" answer
         * (including Ctrl-C/Ctrl-D, which make ok false) abandons. */
        if (!is_yes) {
            kitty_term_puts(c->term, "\r\nConnection abandoned.\r\n");
            kitty_inline_finish(c, SPR_USER_ABORT);
            return;
        }
        c->changed_step = 2;
        free_prompts(c->prompts);
        c->prompts = NULL;
        queue_toplevel_callback(kitty_inline_changed_arm, c);
        return;
      default:
        /* Replace-the-stored-key loop (steps 2 and 3):
         *   "confirmed"        -> replace the stored key and connect
         *   "no" or bare Enter -> connect once, keep the old key
         *   Ctrl-C / Ctrl-D    -> abandon the whole connection (ok == false)
         *   anything else      -> re-ask, so a reflexive "yes" or a typo cannot
         *                         accidentally skip the decision
         * Abandon and decline both terminate, so the loop can never spin. */
        if (!ok) {
            kitty_term_puts(c->term, "\r\nConnection abandoned.\r\n");
            kitty_inline_finish(c, SPR_USER_ABORT);
            return;
        }
        if (is_confirmed) {
            kitty_changed_store_and_connect(c);
            return;
        }
        if (is_no || is_empty) {
            kitty_changed_connect_once(c);
            return;
        }
        c->changed_step = 3;
        free_prompts(c->prompts);
        c->prompts = NULL;
        queue_toplevel_callback(kitty_inline_changed_arm, c);
        return;
    }
}

/* Entry point for the changed-key inline double opt-in. Captures the factual
 * details and defers all terminal work to the step machine above; returns
 * SPR_INCOMPLETE, resolving later through the SSH callback. */
static SeatPromptResult kitty_inline_confirm_changed(
    WinGuiSeat *wgs, SeatDialogText *text, const char *host, int port,
    const char *keytype, const char *keystr,
    void (*callback)(void *ctx, SeatPromptResult result), void *cbctx)
{
    struct kitty_inline_confirm_ctx *c = snew(struct kitty_inline_confirm_ctx);
    c->seat = &wgs->seat;
    c->term = wgs->term;
    c->facts = strbuf_new();
    kitty_facts_to_strbuf(c->facts, text);
    c->promptline = NULL;
    c->abortmsg = NULL;
    c->prompts = NULL;
    c->store_on_yes = false;
    c->host = host ? dupstr(host) : NULL;
    c->keytype = keytype ? dupstr(keytype) : NULL;
    c->keystr = keystr ? dupstr(keystr) : NULL;
    c->port = port;
    c->ssh_callback = callback;
    c->ssh_cbctx = cbctx;
    c->changed_step = 0;
    queue_toplevel_callback(kitty_inline_changed_dispatch, c);
    return SPR_INCOMPLETE;
}

SeatPromptResult win_seat_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *cbctx)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);

    /* Decide modal vs inline. We only need to know whether this is a new or a
     * changed key when at least one of the two host-key flags asks for inline
     * mode; that keeps the common all-modal default path free of a redundant
     * host-key-cache lookup. check_stored_host_key returns 2 when a different
     * key is already cached (changed host); we recompute it locally rather than
     * plumbing a new parameter through the cross-platform seat API. */
    bool new_inline = !GetModalNewHostKeyConfirmationFlag();
    bool changed_inline = !GetModalChangedHostKeyConfirmationFlag();
    if ((new_inline || changed_inline) && kitty_inline_prompts_enabled) {
        bool changed =
            (check_stored_host_key(host, port, keytype, keystr) == 2);
        if (changed ? changed_inline : new_inline) {
            if (kitty_session_is_live(wgs)) {
                /* Rekey: cannot prompt inline (the running program owns the
                 * terminal). Surface the details and abort. */
                return kitty_inline_confirm(
                    wgs, text, NULL,
                    "Host key confirmation cannot be shown during an active "
                    "session. Connection abandoned.\r\n",
                    false, NULL, 0, NULL, NULL, callback, cbctx);
            }
            if (changed) {
                /* Changed key: inline "double opt-in" (branch A). Accept for
                 * this connection, then optionally REPLACE the stored key by
                 * typing "confirmed". See kitty_inline_confirm_changed. */
                return kitty_inline_confirm_changed(
                    wgs, text, host, port, keytype, keystr, callback, cbctx);
            }
            /* New host key: OpenSSH-style typed confirmation. "once" connects
             * without caching the key (parity with the modal Connect Once). */
            return kitty_inline_confirm(
                wgs, text,
                "Are you sure you want to continue connecting "
                "(type \"yes\" to accept and cache the key, \"once\" to connect "
                "without caching, anything else to cancel)? ",
                NULL, true /* store on yes */, host, port, keytype, keystr,
                callback, cbctx);
        }
    }

    /* Classic modal dialog (default). */
    struct hostkey_dialog_ctx ctx[1];
    ctx->text = text;
    ctx->helpctx = helpctx;

    int mbret = ShinyDialogBox(
        hinst, MAKEINTRESOURCE(IDD_HOSTKEY), "PuTTYHostKeyDialog",
        wgs->term_hwnd, HostKeyDialogProc, ctx);
    assert(mbret==IDC_HK_ACCEPT || mbret==IDC_HK_ONCE || mbret==IDCANCEL);
    if (mbret == IDC_HK_ACCEPT) {
        store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    } else if (mbret == IDC_HK_ONCE) {
        return SPR_OK;
    }

    return SPR_USER_ABORT;
}

/*
 * Ask whether the selected algorithm is acceptable (since it was
 * below the configured 'warn' threshold).
 */
SeatPromptResult win_seat_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);

    if (!GetModalWeakKeyConfirmationFlag() && kitty_inline_prompts_enabled) {
        if (kitty_session_is_live(wgs)) {
            return kitty_inline_confirm(
                wgs, text, NULL,
                "Weak-algorithm confirmation cannot be shown during an active "
                "session. Connection abandoned.\r\n",
                false, NULL, 0, NULL, NULL, callback, ctx);
        }
        return kitty_inline_confirm(
            wgs, text,
            "To accept the risk and continue, type \"yes\" "
            "(anything else cancels): ",
            NULL, false /* nothing to store */, NULL, 0, NULL, NULL,
            callback, ctx);
    }

    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = process_seatdialogtext(dlg_text, NULL, text);

    extern int kitty_confirm_box(HWND owner, const char *caption,
                                 const char *text, const char *warn_red);
    int confirmed = kitty_confirm_box(NULL, dlg_title, dlg_text->s, NULL);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (confirmed)
        return SPR_OK;
    else
        return SPR_USER_ABORT;
}

SeatPromptResult win_seat_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);

    if (!GetModalWeakKeyConfirmationFlag() && kitty_inline_prompts_enabled) {
        if (kitty_session_is_live(wgs)) {
            return kitty_inline_confirm(
                wgs, text, NULL,
                "Weak-key confirmation cannot be shown during an active "
                "session. Connection abandoned.\r\n",
                false, NULL, 0, NULL, NULL, callback, ctx);
        }
        return kitty_inline_confirm(
            wgs, text,
            "To accept the risk and continue, type \"yes\" "
            "(anything else cancels): ",
            NULL, false /* nothing to store */, NULL, 0, NULL, NULL,
            callback, ctx);
    }

    strbuf *dlg_text = strbuf_new();
    const char *dlg_title = process_seatdialogtext(dlg_text, NULL, text);

    extern int kitty_confirm_box(HWND owner, const char *caption,
                                 const char *text, const char *warn_red);
    int confirmed = kitty_confirm_box(NULL, dlg_title, dlg_text->s, NULL);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (confirmed)
        return SPR_OK;
    else
        return SPR_USER_ABORT;
}

/*
 * Ask whether to wipe a session log file before writing to it.
 * Returns 2 for wipe, 1 for append, 0 for cancel (don't log).
 */
static int win_gui_askappend(LogPolicy *lp, Filename *filename,
                             void (*callback)(void *ctx, int result),
                             void *ctx)
{
    static const char msgtemplate[] =
        "The session log file \"%.*s\" already exists.\n"
        "You can overwrite it with a new session log,\n"
        "append your session log to the end of it,\n"
        "or disable session logging for this session.\n"
        "Hit Yes to wipe the file, No to append to it,\n"
        "or Cancel to disable logging.";
    char *message;
    char *mbtitle;
    int mbret;

    message = dupprintf(msgtemplate, FILENAME_MAX, filename->utf8path);
    mbtitle = dupprintf("%s Log to File", appname);

    mbret = message_box(NULL, message, mbtitle,
                        MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON3,
                        true, 0);

    socket_reselect_all();

    sfree(message);
    sfree(mbtitle);

    if (mbret == IDYES)
        return 2;
    else if (mbret == IDNO)
        return 1;
    else
        return 0;
}

const LogPolicyVtable win_gui_logpolicy_vt = {
    .eventlog = win_gui_eventlog,
    .askappend = win_gui_askappend,
    .logging_error = win_gui_logging_error,
    .verbose = null_lp_verbose_yes,
};

/*
 * Warn about the obsolescent key file format.
 *
 * Uniquely among these functions, this one does _not_ expect a
 * frontend handle. This means that if PuTTY is ported to a
 * platform which requires frontend handles, this function will be
 * an anomaly. Fortunately, the problem it addresses will not have
 * been present on that platform, so it can plausibly be
 * implemented as an empty function.
 */
void old_keyfile_warning(void)
{
    static const char mbtitle[] = "%s Key File Warning";
    static const char message[] =
        "You are loading an SSH-2 private key which has an\n"
        "old version of the file format. This means your key\n"
        "file is not fully tamperproof. Future versions of\n"
        "%s may stop supporting this private key format,\n"
        "so we recommend you convert your key to the new\n"
        "format.\n"
        "\n"
        "You can perform this conversion by loading the key\n"
        "into PuTTYgen and then saving it again.";

    char *msg, *title;
    msg = dupprintf(message, appname);
    title = dupprintf(mbtitle, appname);

    {
        extern void kitty_info_box(HWND owner, const char *caption,
                                   const char *text, const char *warn_red);
        kitty_info_box(NULL, title, msg, NULL);
    }

    socket_reselect_all();

    sfree(msg);
    sfree(title);
}

static INT_PTR CAConfigProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                            void *ctx)
{
    PortableDialogStuff *pds = (PortableDialogStuff *)ctx;

    switch (msg) {
      case WM_INITDIALOG:
        pds_initdialog_start(pds, hwnd);

        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)));

        centre_window(hwnd);

        pds_create_controls(pds, 0, IDCX_PANELBASE, 3, 3, 3, "Main");
        pds_create_controls(pds, 0, IDCX_STDBASE, 3, 3, 243, "");
        dlg_refresh(NULL, pds->dp);    /* and set up control values */

        pds_initdialog_finish(pds);
        return 0;

      default:
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
    }
}

void show_ca_config_box(dlgparam *dp)
{
    PortableDialogStuff *pds = pds_new(1);

    setup_ca_config_box(pds->ctrlbox);

    ShinyDialogBox(hinst, MAKEINTRESOURCE(IDD_CA_CONFIG), "PuTTYConfigBox",
                   dp ? dp->hwnd : NULL, CAConfigProc, pds);

    pds_free(pds);
}
