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
    }

    pds->dp->errtitle = dupprintf("%s Error", appname);

    pds->initialised = false;

    return pds;
}

static void pds_free(PortableDialogStuff *pds)
{
    ctrl_free_box(pds->ctrlbox);

    dp_cleanup(pds->dp);

    for (size_t i = 0; i < pds->nctrltrees; i++)
        winctrl_cleanup(&pds->ctrltrees[i]);
    sfree(pds->ctrltrees);

    sfree(pds);
}

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
                                  hwnd, ((LPHELPINFO)lParam)->iCtrlId))
            MessageBeep(0);
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
#define CFGBOX_H             316   /* == IDD_MAINBOX height in the template */
#define CFGBOX_BUTTONROW_DU  (CFGBOX_H - 17)   /* top of the button row */
#define CFGBOX_TREE_DU       (CFGBOX_BUTTONROW_DU - 17)  /* tree height */
/* Width kept clear at the right of every panel for the scroll bar. */
#define CFGBOX_SCROLLGUTTER_DU 10

enum {
    IDCX_ABOUT = IDC_ABOUT,
    IDCX_TVSTATIC,
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

static HTREEITEM treeview_insert(struct treeview_faff *faff,
                                 int level, char *text, char *path)
{
    TVINSERTSTRUCT ins;
    int i;
    HTREEITEM newitem;
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
     * eye. scroll_y is kept per panel, so switching away and back returns to
     * the same place.
     */
    int content_h;
    int scroll_y;
};

static struct kitty_cfg_panel **kitty_cfg_panels = NULL;
static size_t kitty_cfg_npanels = 0, kitty_cfg_panelsize = 0;
static struct kitty_cfg_panel *kitty_cfg_active_panel = NULL;
/* Top of the fixed button strip, in client pixels - the floor of the panel
 * area. Set once the row exists, because it moves with cb_extra_du. */
static int kitty_cfg_buttonrow_top = 0;

static struct kitty_cfg_panel *kitty_cfg_panel_find(const char *path)
{
    for (size_t i = 0; i < kitty_cfg_npanels; i++)
        if (!strcmp(kitty_cfg_panels[i]->path, path))
            return kitty_cfg_panels[i];
    return NULL;
}

/* Show or hide a cached panel's windows, and swap its shortcuts in or out of
 * the dlgparam table with it. */
static void kitty_cfg_panel_show(struct dlgparam *dp,
                                 struct kitty_cfg_panel *p, bool show)
{
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(dp->hwnd, c->base_id + k);
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
        if (show)
            winctrl_add_shortcuts(dp, c);
        else
            winctrl_rem_shortcuts(dp, c);
    }
}

/*
 * ------------------------------------------------------------ panel scrolling
 *
 * Segoe UI 9 made every row about 15% taller, which took the box past the
 * height that fits a 1080p laptop at 150% (see the note in
 * windows/putty-common.rc2). The panel area scrolls instead of the window
 * growing - and the saved-session list is configurable in rows, so some
 * panels could outgrow any fixed height anyway.
 *
 * The controls are children of the DIALOG, not of a container window, so
 * there is nothing to scroll: scrolling means moving the panel's own controls
 * and leaving the tree, the button row and the scroll bar where they are.
 * That is why every function here works from one panel's ctrls list.
 */

/* The rectangle a panel's controls live in, in client coordinates. Derived
 * from the same numbers the panels are laid out with, so the two cannot drift:
 * left 100 DLU, right border 3, top 13, down to the button row. */
static void kitty_cfg_panel_rect(HWND hwnd, RECT *out)
{
    RECT dlu = { 100, 13, 3 + CFGBOX_SCROLLGUTTER_DU, 0 };
    RECT client;

    MapDialogRect(hwnd, &dlu);
    GetClientRect(hwnd, &client);
    out->left = dlu.left;
    out->top = dlu.top;
    out->right = client.right - dlu.right;
    out->bottom = client.bottom;

    /*
     * The button row is the floor, and it is taken from the layout constant,
     * NOT by looking a button up.
     *
     * GetDlgItem(hwnd, IDOK) was the obvious way to find it and it is wrong:
     * this box's buttons are built by ctrl_pushbutton and get generated ids
     * (1010 and up, plus IDCANCEL), so there is no control with id 1. The
     * lookup returned NULL, the floor silently stayed at the bottom of the
     * window, and nothing was clipped above the buttons at all - which is the
     * whole point of this rectangle.
     */
    if (kitty_cfg_buttonrow_top > out->top)
        out->bottom = kitty_cfg_buttonrow_top - 4;
}

/* How tall a panel's contents are: the lowest bottom edge of any of its
 * controls, measured from the top of the panel area. Measured, not predicted -
 * a panel's height is the sum of whatever its handlers built. */
static int kitty_cfg_panel_measure(HWND hwnd, struct kitty_cfg_panel *p)
{
    RECT area;
    int bottom = 0;

    kitty_cfg_panel_rect(hwnd, &area);
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(hwnd, c->base_id + k);
            RECT r;
            if (!item || !GetWindowRect(item, &r))
                continue;
            MapWindowPoints(NULL, hwnd, (POINT *)&r, 2);
            if (r.bottom > bottom)
                bottom = r.bottom;
        }
    }
    if (!bottom)
        return 0;
    /* Plus a little air at the foot, so the last control does not sit hard
     * against the button row when scrolled to the end. */
    return (bottom - area.top) + p->scroll_y + 4;
}

/*
 * Hide whatever has scrolled out of the panel area.
 *
 * The panel's controls are children of the DIALOG, so nothing clips them:
 * scrolled far enough, a control simply carries on drawing over the button
 * row. There is no container window to clip against - giving the panel one
 * would mean re-parenting every control, and every dlg_* call in the codebase
 * looks its control up with GetDlgItem(dp->hwnd, id).
 *
 * So the clipping is done by hand, with a WINDOW REGION on any control that
 * straddles the boundary. A half-scrolled row is then drawn as half a row,
 * cut exactly at the edge - not hidden (which made rows vanish while still
 * half visible) and not left to paint across the button strip below. That
 * strip is a fixed area of its own and must never be scrolled into, whatever
 * the panel is doing.
 */
static void kitty_cfg_panel_clip(HWND hwnd, struct kitty_cfg_panel *p)
{
    RECT area;

    if (!p)
        return;
    kitty_cfg_panel_rect(hwnd, &area);
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(hwnd, c->base_id + k);
            RECT r;
            if (!item || !GetWindowRect(item, &r))
                continue;
            MapWindowPoints(NULL, hwnd, (POINT *)&r, 2);

            if (r.bottom <= area.top || r.top >= area.bottom) {
                /* Wholly outside. Hidden - and the keyboard must not be left
                 * on it, or focus goes somewhere the reader cannot see. */
                if (GetFocus() == item)
                    SetFocus(GetDlgItem(hwnd, IDCX_TREEVIEW));
                SetWindowPos(item, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE | SWP_NOREDRAW | SWP_HIDEWINDOW);
                continue;
            }

            if (r.top < area.top || r.bottom > area.bottom) {
                /* Straddling: cut it at the boundary. The region is in the
                 * control's OWN coordinates, and SetWindowRgn takes ownership
                 * of it - it must not be deleted here. */
                int top = (area.top > r.top ? area.top : r.top) - r.top;
                int bot = (area.bottom < r.bottom ? area.bottom : r.bottom)
                          - r.top;
                HRGN rgn = CreateRectRgn(0, top, r.right - r.left, bot);
                if (rgn)
                    SetWindowRgn(item, rgn, FALSE);
            } else {
                /* Fully inside: no region, so it draws exactly as it would on
                 * a panel that never scrolls. */
                SetWindowRgn(item, NULL, FALSE);
            }
            SetWindowPos(item, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
        }
    }
}

/* Move a panel's controls to match a scroll position. */
static void kitty_cfg_panel_scroll_to(HWND hwnd, struct kitty_cfg_panel *p,
                                      int newy)
{
    RECT area;
    int maxy, delta;
    HDWP dwp;

    kitty_cfg_panel_rect(hwnd, &area);
    maxy = p->content_h - (area.bottom - area.top);
    if (maxy < 0)
        maxy = 0;
    if (newy < 0)
        newy = 0;
    if (newy > maxy)
        newy = maxy;
    delta = newy - p->scroll_y;
    if (!delta)
        return;

    /* One DeferWindowPos batch: moving them one at a time repaints the area
     * once per control, which reads as a shudder rather than a scroll. */
    dwp = BeginDeferWindowPos((int)p->nctrls * 2 + 4);
    for (size_t i = 0; i < p->nctrls; i++) {
        struct winctrl *c = p->ctrls[i];
        for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(hwnd, c->base_id + k);
            RECT r;
            if (!item || !GetWindowRect(item, &r))
                continue;
            MapWindowPoints(NULL, hwnd, (POINT *)&r, 2);
            /*
             * NOT SWP_NOREDRAW. Suppressing the redraw looks like the way to
             * stop the flicker, and it leaves the pixels a control has moved
             * AWAY from on the screen - the ghosted duplicate rows and stray
             * glyph fragments that made the first attempt unusable. Windows
             * invalidates both the old and the new rectangle when it is
             * allowed to; the flicker is dealt with by WS_CLIPCHILDREN on the
             * dialog instead, which stops the background being painted under
             * the controls in the first place.
             */
            if (dwp)
                dwp = DeferWindowPos(dwp, item, NULL,
                                     r.left, r.top - delta, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER |
                                     SWP_NOACTIVATE);
            else
                SetWindowPos(item, NULL, r.left, r.top - delta, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (dwp)
        EndDeferWindowPos(dwp);

    p->scroll_y = newy;
    SetScrollPos(GetDlgItem(hwnd, IDCX_PANELSCROLL), SB_CTL, newy, TRUE);
    kitty_cfg_panel_clip(hwnd, p);
    /*
     * ONE repaint, of the panel area only, children included, done now rather
     * than left for the next idle - a scroll that paints late reads as a
     * stutter.
     *
     * RDW_ERASE, not RDW_NOERASE: the background a control has just moved
     * AWAY from has to be repainted, or its old pixels stay on screen as
     * artefacts. The flash that erasing would normally cause is handled by
     * WS_CLIPCHILDREN on the dialog (set in WM_INITDIALOG), which stops the
     * erase from painting under the controls themselves.
     */
    RedrawWindow(hwnd, &area, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

/*
 * Fit the scroll bar to the panel now showing: sized and shown when the panel
 * is taller than the area, hidden outright when it is not.
 */
static void kitty_cfg_panel_scrollbar(HWND hwnd, struct kitty_cfg_panel *p)
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
    if (p->content_h <= areah) {
        /* Nothing to scroll: the bar goes away entirely, and any leftover
         * offset is undone so the panel is not left part-scrolled. */
        if (p->scroll_y)
            kitty_cfg_panel_scroll_to(hwnd, p, 0);
        ShowWindow(sb, SW_HIDE);
        return;
    }

    width = GetSystemMetrics(SM_CXVSCROLL);
    /* In the gutter reserved for it, just outside the panel proper. */
    SetWindowPos(sb, NULL, area.right + 2, area.top,
                 width, areah, SWP_NOZORDER | SWP_NOACTIVATE);

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = p->content_h - 1;
    si.nPage = areah;
    si.nPos = p->scroll_y;
    SetScrollInfo(sb, SB_CTL, &si, TRUE);
    ShowWindow(sb, SW_SHOW);
    kitty_cfg_panel_clip(hwnd, p);
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

/* Create a panel's controls (visible, shortcuts registered - creation IS
 * showing) and record it in the cache. The controls are laid out into a
 * scratch tree, then moved one by one into the shared TREE_PANEL tree; the
 * drain preserves creation order because winctrl_findbyindex walks the byid
 * tree and ids ascend as they are handed out. */
static struct kitty_cfg_panel *kitty_cfg_panel_create(
    PortableDialogStuff *pds, const char *path)
{
    struct kitty_cfg_panel *p = snew(struct kitty_cfg_panel);
    p->path = dupstr(path);
    p->base_id = IDCX_PANELBASE +
        (int)kitty_cfg_npanels * KITTY_PANEL_ID_STRIDE;
    p->ctrls = NULL;
    p->nctrls = p->ctrlsize = 0;

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
    ctlposinit(&cp, pds->dp->hwnd, 100, 3 + CFGBOX_SCROLLGUTTER_DU, 13);
    int id = p->base_id;
    for (int index = -1; (index = ctrl_find_path(
                              pds->ctrlbox, p->path, index)) >= 0 ;) {
        struct controlset *s = pds->ctrlbox->ctrlsets[index];
        winctrl_layout(pds->dp, &scratch, &cp, s, &id);
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

    sgrowarray(kitty_cfg_panels, kitty_cfg_panelsize, kitty_cfg_npanels);
    kitty_cfg_panels[kitty_cfg_npanels++] = p;
    /* These controls did not exist when the box was themed, so nothing that
     * has to be SENT to a control has reached them: without this a cached
     * panel comes up in the classic colours the first time it is shown. */
    kitty_theme_refresh(pds->dp->hwnd);
    /* Measured now, while the controls are at their unscrolled positions. */
    p->content_h = kitty_cfg_panel_measure(pds->dp->hwnd, p);
    return p;
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
const char *kitty_cfgbox_wanted_panel(void);         /* kitty_config.c / stub:
                                                      * panel path to open on,
                                                      * or NULL for the first */
#define KITTY_WORKPLACE_POLL_TIMER 8730
#define KITTY_PANEL_WARMUP_TIMER 8731
bool kitty_config_select_root_folder(dlgparam *dp); /* kitty_config.c / stub */
void kitty_config_end_folder_rename(dlgparam *dp);  /* kitty_config.c / stub */
static HHOOK kitty_cfg_kbdhook = NULL;
static HWND kitty_cfg_hwnd = NULL;
static HWND kitty_cfg_treeview = NULL;
static HTREEITEM kitty_cfg_sessionitem = NULL;
static dlgparam *kitty_cfg_dp = NULL;

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
                if (kitty_cfg_treeview && kitty_cfg_sessionitem)
                    TreeView_SelectItem(kitty_cfg_treeview,
                                        kitty_cfg_sessionitem);
                dlg_set_focus_later(ctrl, kitty_cfg_dp);
                return 1;               /* handled: swallow the keystroke */
            }
        }
    }
    return CallNextHookEx(kitty_cfg_kbdhook, code, wParam, lParam);
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
      case WM_EXITSIZEMOVE:
        kitty_cfgbox_save_pos(hwnd);   /* remember where the user dragged it */
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_DESTROY:
        /* Robust backstop: capture the final position at close, regardless of
         * how the box was moved (WM_EXITSIZEMOVE only fires on interactive drag). */
        kitty_cfgbox_save_pos(hwnd);
        kitty_cfg_panel_cache_reset();  /* the windows die with the dialog */
        /* KiTTY: tear down the Ctrl+F session-search jump with its dialog. */
        if (kitty_cfg_hwnd == hwnd) {
            if (kitty_cfg_kbdhook) {
                UnhookWindowsHookEx(kitty_cfg_kbdhook);
                kitty_cfg_kbdhook = NULL;
            }
            kitty_cfg_hwnd = NULL;
            kitty_cfg_treeview = NULL;
            kitty_cfg_sessionitem = NULL;
            kitty_cfg_dp = NULL;
        }
        return pds_default_dlgproc(pds, hwnd, msg, wParam, lParam);
      case WM_INITDIALOG: {
        pds_initdialog_start(pds, hwnd);

        /* WS_CLIPCHILDREN: the dialog stops painting the background under its
         * own controls. Without it every repaint of the panel area erases the
         * whole rectangle first and the controls draw on top of it, which is
         * the flash behind a scroll. */
        SetWindowLong(hwnd, GWL_STYLE,
                      GetWindowLong(hwnd, GWL_STYLE) | WS_CLIPCHILDREN);

        /* KiTTY: the saved-session list height is configurable via kitty.ini
         * [ConfigBox] height (GetConfigBoxHeight(), default 16 = stock fit), and the whole
         * box via [ConfigBox] windowheight. A taller list needs the button row,
         * the treeview and the window itself to grow to match; cb_extra_du is
         * that growth in dialog units (0 at/below the stock-fit height). */
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
                            CFGBOX_BUTTONROW_DU + cb_extra_du, ""); /* buttons row */
        /* Where that row landed, in pixels: the floor of the panel area, and
         * the line nothing scrolled may cross - you shall not pass! Taken
         * from the same units the row was created with, so the two cannot
         * disagree. */
        {
            RECT br = { 0, CFGBOX_BUTTONROW_DU + cb_extra_du, 0, 0 };
            MapDialogRect(hwnd, &br);
            kitty_cfg_buttonrow_top = br.top;
        }

        SendMessage(hwnd, WM_SETICON, (WPARAM) ICON_BIG,
                    (LPARAM) LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)));

        /* KiTTY: grow the window to fit a taller list, or to an explicit
         * windowheight (px, DPI-scaled). Done before centring so the box is
         * placed at its final size. */
        {
            extern int GetConfigBoxWindowHeight(void);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int cur_h = wr.bottom - wr.top, want_h = cur_h;
            int wh = GetConfigBoxWindowHeight();
            if (wh > 0) {
                HDC hdc = GetDC(hwnd);
                double sy = GetDeviceCaps(hdc, LOGPIXELSY) / 96.0;
                ReleaseDC(hwnd, hdc);
                want_h = (int)(wh * sy);
            } else if (cb_extra_du > 0) {
                RECT er = { 0, 0, 0, cb_extra_du };
                MapDialogRect(hwnd, &er);
                want_h = cur_h + er.bottom;
            }
            if (want_h != cur_h)
                SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, want_h,
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
         * Create the tree view.
         */
        {
            RECT r;
            WPARAM font;
            HWND tvstatic;

            r.left = 3;
            r.right = r.left + 95;
            r.top = 3;
            r.bottom = r.top + 10;
            MapDialogRect(hwnd, &r);
            tvstatic = CreateWindowEx(0, "STATIC", "Cate&gory:",
                                      WS_CHILD | WS_VISIBLE,
                                      r.left, r.top,
                                      r.right - r.left, r.bottom - r.top,
                                      hwnd, (HMENU) IDCX_TVSTATIC, hinst,
                                      NULL);
            font = SendMessage(hwnd, WM_GETFONT, 0, 0);
            SendMessage(tvstatic, WM_SETFONT, font, MAKELPARAM(true, 0));

            r.left = 3;
            r.right = r.left + 95;
            r.top = 13;
            r.bottom = r.top + CFGBOX_TREE_DU + cb_extra_du; /* KiTTY: (was
                                       * 219); grows with [ConfigBox] height so
                                       * Bell/Data/Appearance panels aren't cut */
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
            tvfaff.treeview = treeview;
            memset(tvfaff.lastat, 0, sizeof(tvfaff.lastat));
        }

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

            /*
             * Put the treeview selection on to the first panel in the
             * ctrlbox - or on the panel something asked us to open on.
             */
            /* hfirst stays the FIRST panel (the Session one) because the Ctrl+F
             * jump below is anchored to it; only what we select changes. */
            HTREEITEM hsel = hwanted ? hwanted : hfirst;
            char *selpath = hwanted ? wantedpath : firstpath;
            TreeView_SelectItem(treeview, hsel);

            /* KiTTY: arm the Ctrl+F session-search jump (first tree item ==
             * the Session panel). Only when this dialog's ctrlbox actually
             * registered a session box — stock variants return NULL. */
            if (kitty_config_session_filter_ctrl()) {
                kitty_cfg_hwnd = hwnd;
                kitty_cfg_dp = pds->dp;
                kitty_cfg_treeview = treeview;
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
                kitty_cfg_panel_measure(hwnd, kitty_cfg_active_panel);
            kitty_cfg_panel_scrollbar(hwnd, kitty_cfg_active_panel);
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
            int page, y = p->scroll_y;
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

      case WM_MOUSEWHEEL:
        /* The wheel scrolls the panel wherever the pointer is, as long as the
         * panel HAS a scroll bar - a panel that fits does not move under the
         * wheel, which is what the reader expects. */
        if (kitty_cfg_active_panel) {
            HWND sb = GetDlgItem(hwnd, IDCX_PANELSCROLL);
            if (sb && IsWindowVisible(sb)) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                kitty_cfg_panel_scroll_to(
                    hwnd, kitty_cfg_active_panel,
                    kitty_cfg_active_panel->scroll_y - (delta / WHEEL_DELTA) * 48);
                return 0;
            }
        }
        return 0;
      case WM_TIMER:
        if ((UINT_PTR)wParam == KITTY_PANEL_WARMUP_TIMER) {
            if (!kitty_cfg_warmup_step(pds))
                KillTimer(hwnd, KITTY_PANEL_WARMUP_TIMER);
            return 0;
        }
        if ((UINT_PTR)wParam == KITTY_WORKPLACE_POLL_TIMER) {
            kitty_cfgbox_workplace_poll(pds->dp);
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

            i = TreeView_GetSelection(((LPNMHDR) lParam)->hwndFrom);

            item.hItem = i;
            item.pszText = buffer;
            item.cchTextMax = sizeof(buffer);
            item.mask = TVIF_TEXT | TVIF_PARAM;
            TreeView_GetItem(((LPNMHDR) lParam)->hwndFrom, &item);
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
                kitty_cfg_panel_scrollbar(hwnd, newpanel);

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

    int mbret = MessageBox(NULL, dlg_text->s, dlg_title,
                           MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (mbret == IDYES)
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

    int mbret = MessageBox(NULL, dlg_text->s, dlg_title,
                           MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    socket_reselect_all();
    strbuf_free(dlg_text);

    if (mbret == IDYES)
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

    MessageBox(NULL, msg, title, MB_OK);

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
