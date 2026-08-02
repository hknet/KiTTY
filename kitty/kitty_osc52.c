/*
 * KiTTY OSC 52 clipboard-READ support: the platform half.
 *
 * The permission logic lives in terminal/terminal.c, which is cross-platform and
 * stays free of Win32 and of dialog code. Everything that needs a window, the
 * Windows clipboard, or the registry is here, reached through the seams declared
 * at the top of terminal.c. The split is not cosmetic: it is what lets
 * test_osc52 exercise the whole permission engine with stubs, so the rules about
 * expiry, rate limits and focus are covered by tests that never open a window.
 *
 * Design: design/TASK_clipboard_read_permission.md.
 *
 * Compiled into the kitty and kitty_portable targets only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "terminal.h"
#include "putty-rc.h"          /* dialog resource ids (kitty_rc_additions.h) */

#include <windows.h>

#include "kitty.h"

extern HWND MainHwnd;          /* kitty.c: the terminal window */
void kitty_refresh_title(void);        /* windows/window.c */

/* ------------------------------------------------------------------------
 * Reading the local clipboard
 * ------------------------------------------------------------------------ */

/*
 * Fetch the clipboard as wide text. Returns NULL when there is nothing to send,
 * which includes the case where the clipboard holds something that is not text at
 * all - an image, a file list. We do not offer to send those: OSC 52 has no way
 * to describe a content type, so anything we sent would be a lie about what it
 * is. (OSC 5522 does carry a MIME type, and that is where non-text belongs.)
 *
 * *len is the number of wide characters, not counting the terminating NUL.
 */
wchar_t *kitty_osc52_get_clipboard(int *len)
{
    HANDLE h;
    void *p;
    wchar_t *out = NULL;
    size_t n;

    if (len)
        *len = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return NULL;
    if (!OpenClipboard(NULL))
        return NULL;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h && (p = GlobalLock(h)) != NULL) {
        /* GlobalSize is the allocation, which can be larger than the string;
         * wcsnlen bounds the scan so a clipboard entry without a terminator
         * cannot walk off the end of the block. */
        n = wcsnlen((const wchar_t *)p, GlobalSize(h) / sizeof(wchar_t));
        out = snewn(n + 1, wchar_t);
        memcpy(out, p, n * sizeof(wchar_t));
        out[n] = L'\0';
        if (len)
            *len = (int)n;
        GlobalUnlock(h);
    }
    CloseClipboard();
    if (out && (!len || *len == 0)) {
        sfree(out);
        out = NULL;
    }
    return out;
}

/*
 * Send a complete, already-built sequence to the host. Used by both OSC 52 and
 * OSC 5522; the caller builds the whole thing, because the two protocols frame
 * their replies differently and this end has no business knowing which is which.
 *
 * It is a seam rather than an ldisc_send() inside terminal.c so that the one event
 * that must never happen by accident - the clipboard leaving the machine - is a
 * single function, which the tests can count and which is the obvious place to put
 * a breakpoint when asking "did it actually go out?".
 */
void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len)
{
    if (!term || !term->ldisc || !data || !len)
        return;                        /* no connection to reply down */
    ldisc_send(term->ldisc, data, (int)len, false);
}

/* ------------------------------------------------------------------------
 * The permission dialog
 * ------------------------------------------------------------------------ */

/* What the dialog is being asked about, and what it answers. One instance,
 * stack-allocated by the caller and passed through DialogBoxParam. */
struct osc52_ask {
    Terminal *term;
    const wchar_t *clip;
    int clip_len;
    const char *claim;         /* program's self-description, or NULL on OSC 52 */

    int grant;                 /* OSC52_GRANT_* */
    bool always_deny;
    bool allowed;

    int seconds_left;          /* countdown; 0 means no timeout */
    bool revealed;             /* has View been pressed? */
};

/* Match terminal.c's enum. Kept in step by hand: it is four values that have not
 * changed since the design, and exporting it through a header would drag the
 * cross-platform file's internals into every Windows TU that includes putty.h. */
enum {
    OSC52_GRANT_ONCE,
    OSC52_GRANT_MINUTES,
    OSC52_GRANT_REQUESTS,
    OSC52_GRANT_SESSION,
};

#define OSC52_ASK_TIMER 1

/*
 * The masked summary: how much there is, and just enough of the start to
 * recognise it by. Masked is the default and it is the point - a dialog appearing
 * must not put the clipboard on screen for a shoulder, a screen-share or a
 * screenshot to collect, least of all when the answer is about to be no.
 *
 * A password manager's entry is short and single-line, which is exactly the shape
 * that would be fully revealed by "show the first twenty characters", so the
 * preview is capped hard at three and never shows more of a short string than a
 * long one.
 */
static char *osc52_mask_summary(const wchar_t *clip, int clip_len)
{
    int lines = 1, i, shown;
    char head[16];
    wchar_t wh[4];
    strbuf *sb = strbuf_new_nm();
    char *ret;

    for (i = 0; i < clip_len; i++)
        if (clip[i] == L'\n')
            lines++;

    shown = clip_len < 3 ? clip_len : 3;
    for (i = 0; i < shown; i++)
        wh[i] = (clip[i] == L'\r' || clip[i] == L'\n' ||
                 clip[i] == L'\t') ? L' ' : clip[i];
    wh[shown] = L'\0';
    {
        char *u = encode_wide_string_as_utf8(wh);
        strncpy(head, u, sizeof(head) - 1);
        head[sizeof(head) - 1] = '\0';
        smemclr(u, strlen(u));
        sfree(u);
    }

    put_fmt(sb, "It would send %d character%s on %d line%s, starting \"%s\"...",
            clip_len, clip_len == 1 ? "" : "s",
            lines, lines == 1 ? "" : "s", head);
    put_dataz(sb, "\r\nThe rest is hidden until you press View.");
    smemclr(head, sizeof(head));
    ret = strbuf_to_str(sb);
    return ret;
}

/* Where the request came from, so somebody with a dozen windows open knows
 * which one is asking. */
static char *osc52_where(Terminal *term)
{
    const char *sess = conf_get_str(term->conf, CONF_sessionname);
    const char *host = conf_get_str(term->conf, CONF_host);
    if (sess && *sess)
        return dupprintf("Session \"%s\" (%s)", sess, host && *host ? host : "?");
    return dupprintf("Unsaved session to %s", host && *host ? host : "?");
}

static void osc52_set_countdown(HWND hwnd, struct osc52_ask *ask)
{
    char *s;
    if (ask->seconds_left <= 0) {
        SetDlgItemText(hwnd, IDC_O52_COUNTDOWN, "");
        return;
    }
    /* Say what the timeout DOES, not just that there is one: "no decision" is
     * the honest description, because a timeout is not the user saying no - it is
     * the user saying nothing, and nothing gets remembered. */
    s = dupprintf("No answer in %d s = refused, and nothing remembered.",
                  ask->seconds_left);
    SetDlgItemText(hwnd, IDC_O52_COUNTDOWN, s);
    sfree(s);
}

static INT_PTR CALLBACK osc52_ask_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    struct osc52_ask *ask = (struct osc52_ask *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_INITDIALOG: {
        char *s;
        int mins, reqs;

        ask = (struct osc52_ask *)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)ask);

        SetDlgItemText(hwnd, IDC_O52_WHAT,
                       "This server wants to READ your clipboard and send the "
                       "contents back to it. Your clipboard may hold a password.");

        s = osc52_where(ask->term);
        SetDlgItemText(hwnd, IDC_O52_WHERE, s);
        sfree(s);

        /*
         * The program's claim about itself, on OSC 5522 only, and worded as a
         * claim: the text comes from the server, so anything can call itself
         * "nvim". Control characters are stripped and the length capped, because
         * it must not be able to forge parts of this dialog.
         */
        if (ask->claim && *ask->claim) {
            char buf[80];
            size_t i, j = 0;
            for (i = 0; ask->claim[i] && j < sizeof(buf) - 1; i++) {
                unsigned char c = (unsigned char)ask->claim[i];
                if (c >= ' ' && c != 0x7f)
                    buf[j++] = (char)c;
            }
            buf[j] = '\0';
            s = dupprintf("A program calling itself \"%s\"", buf);
            SetDlgItemText(hwnd, IDC_O52_CLAIM, s);
            sfree(s);
        } else {
            /* An OSC 52 read carries no identity at all, and saying so is worth
             * a line: it is why "remember this program" is not on offer. */
            SetDlgItemText(hwnd, IDC_O52_CLAIM,
                           "The request does not say which program sent it, and "
                           "cannot.");
        }

        s = osc52_mask_summary(ask->clip, ask->clip_len);
        SetDlgItemText(hwnd, IDC_O52_SUMMARY, s);
        smemclr(s, strlen(s));
        sfree(s);

        /* The two configurable durations are labelled from the settings, so the
         * dialog cannot claim ten minutes while the setting says five. */
        mins = conf_get_int(ask->term->conf, CONF_osc52_read_minutes);
        if (mins <= 0) mins = 10;
        s = dupprintf("the next %d &minute%s", mins, mins == 1 ? "" : "s");
        SetDlgItemText(hwnd, IDC_O52_MINUTES, s);
        sfree(s);

        reqs = conf_get_int(ask->term->conf, CONF_osc52_read_requests);
        if (reqs <= 0) reqs = 25;
        s = dupprintf("the next %d re&quests", reqs);
        SetDlgItemText(hwnd, IDC_O52_REQUESTS, s);
        sfree(s);

        /*
         * "Just this request" starts selected. Leaving all four blank looks more
         * neutral and is actually worse: the code below falls back to ONCE when
         * nothing is picked, so the narrowest answer was in force with nothing on
         * screen saying so - an invisible default, which is the one thing a
         * security dialog must not have. Showing it nudges towards nothing,
         * because it IS the narrowest option, and it makes the consequence of
         * pressing either button visible before it is pressed.
         */
        CheckRadioButton(hwnd, IDC_O52_ONCE, IDC_O52_SESSION, IDC_O52_ONCE);

        ask->seconds_left = conf_get_int(ask->term->conf, CONF_osc52_read_timeout);
        osc52_set_countdown(hwnd, ask);
        if (ask->seconds_left > 0)
            SetTimer(hwnd, OSC52_ASK_TIMER, 1000, NULL);

        /* Focus lands on Deny, not on Allow. */
        SetFocus(GetDlgItem(hwnd, IDCANCEL));
        SetForegroundWindow(hwnd);
        return FALSE;                  /* we set the focus ourselves */
      }

      case WM_TIMER:
        if (wParam == OSC52_ASK_TIMER && ask) {
            if (--ask->seconds_left <= 0) {
                KillTimer(hwnd, OSC52_ASK_TIMER);
                /* Nobody decided. Refuse this request and remember nothing. */
                ask->allowed = false;
                ask->grant = OSC52_GRANT_ONCE;
                ask->always_deny = false;
                EndDialog(hwnd, 0);
                return TRUE;
            }
            osc52_set_countdown(hwnd, ask);
        }
        return TRUE;

      case WM_COMMAND:
        if (!ask)
            break;
        switch (LOWORD(wParam)) {
          case IDC_O52_VIEW:
            if (!ask->revealed) {
                /* Reveal in place, in a read-only box. There is no Windows
                 * clipboard viewer to hand this off to - clipbrd.exe went away
                 * after XP, and Win+V is the history panel, not a viewer for one
                 * payload. */
                HWND ed = GetDlgItem(hwnd, IDC_O52_PREVIEW);
                SetWindowTextW(ed, ask->clip);
                ShowWindow(ed, SW_SHOW);
                EnableWindow(GetDlgItem(hwnd, IDC_O52_VIEW), FALSE);
                ask->revealed = true;
            }
            return TRUE;

          case IDC_O52_ALLOW:
          case IDCANCEL: {
            ask->allowed = (LOWORD(wParam) == IDC_O52_ALLOW);
            ask->always_deny =
                (IsDlgButtonChecked(hwnd, IDC_O52_ALWAYSDENY) == BST_CHECKED);

            if (IsDlgButtonChecked(hwnd, IDC_O52_SESSION) == BST_CHECKED)
                ask->grant = OSC52_GRANT_SESSION;
            else if (IsDlgButtonChecked(hwnd, IDC_O52_REQUESTS) == BST_CHECKED)
                ask->grant = OSC52_GRANT_REQUESTS;
            else if (IsDlgButtonChecked(hwnd, IDC_O52_MINUTES) == BST_CHECKED)
                ask->grant = OSC52_GRANT_MINUTES;
            else
                ask->grant = OSC52_GRANT_ONCE;

            /*
             * "For the rest of the session" is the widest thing on offer and the
             * only one with no bound on it at all, so ALLOWING it is gated behind
             * a second, explicit confirmation. Denying for the session is not:
             * refusing only ever takes permission away, and making it harder to
             * refuse would be the wrong way round.
             */
            if (ask->allowed && ask->grant == OSC52_GRANT_SESSION) {
                if (MessageBox(hwnd,
                               "For the rest of this session, this server may "
                               "read your clipboard whenever it asks - not once, "
                               "but every time.\n\n"
                               "That includes anything you copy later, such as a "
                               "password from your password manager. It still "
                               "stops while the window has no focus, and there is "
                               "still a limit on how often it is handed over.\n\n"
                               "Allow that?",
                               "KiTTY - allow for the whole session?",
                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return TRUE;       /* back to the dialog, nothing decided */
            }

            /* Ticking "always deny for this host" while pressing Allow is a
             * contradiction. Take the tick as the real intent: it is the
             * safer of the two readings, and it is the one the user typed
             * deliberately rather than the button they may have aimed at. */
            if (ask->always_deny)
                ask->allowed = false;

            KillTimer(hwnd, OSC52_ASK_TIMER);
            EndDialog(hwnd, 0);
            return TRUE;
          }
        }
        break;

      case WM_CLOSE:
        /* Closing the dialog means no, and remembers nothing. */
        if (ask) {
            ask->allowed = false;
            ask->grant = OSC52_GRANT_ONCE;
            ask->always_deny = false;
            KillTimer(hwnd, OSC52_ASK_TIMER);
        }
        EndDialog(hwnd, 0);
        return TRUE;

      case WM_DESTROY:
        if (ask && ask->revealed) {
            /* Do not leave the clipboard sitting in a control's text buffer for
             * the rest of the process's life. */
            HWND ed = GetDlgItem(hwnd, IDC_O52_PREVIEW);
            if (ed)
                SetWindowTextW(ed, L"");
        }
        break;
    }
    return FALSE;
}

bool kitty_osc52_read_dialog(Terminal *term, const wchar_t *clip, int clip_len,
                             const char *claim, int *grant, bool *always_deny)
{
    struct osc52_ask ask;

    memset(&ask, 0, sizeof(ask));
    ask.term = term;
    ask.clip = clip;
    ask.clip_len = clip_len;
    ask.claim = claim;
    ask.grant = OSC52_GRANT_ONCE;
    ask.allowed = false;

    /*
     * Bring the window to the front first. The request only gets this far when
     * the window has focus, but it may still be behind something, and a dialog
     * about the clipboard that appears without its window is a dialog nobody can
     * place.
     */
    if (MainHwnd) {
        ShowWindow(MainHwnd, SW_SHOWNA);
        SetForegroundWindow(MainHwnd);
    }

    DialogBoxParam(hinst, MAKEINTRESOURCE(IDD_OSC52READ), MainHwnd,
                   osc52_ask_proc, (LPARAM)&ask);

    if (grant)
        *grant = ask.grant;
    if (always_deny)
        *always_deny = ask.always_deny;
    return ask.allowed;
}

/* ------------------------------------------------------------------------
 * "Always deny for this host"
 * ------------------------------------------------------------------------ */

/*
 * Write the refusal into the saved session, so this host never asks again.
 *
 * Returns false when there is nothing to write into - an unnamed "type a host and
 * go" window, or Default Settings, which must not be rewritten from one window's
 * state or every later session inherits the change. The caller then tells the
 * user and keeps the refusal for the rest of the window: we do not quietly build
 * a hidden host list, because an invisible permission record is exactly what this
 * whole design exists to avoid.
 *
 * This goes through save_settings(), the same path Change Settings and
 * save-on-exit use, so it honours the ini/registry backend instead of assuming
 * the registry. It does save the window's other live settings along with it -
 * they ARE this session's settings, and the alternative is a bespoke
 * single-value writer that would have to know about both backends.
 */
bool kitty_osc52_save_deny_for_host(Terminal *term)
{
    const char *name;
    char *err;

    if (!term || !term->conf)
        return false;
    name = conf_get_str(term->conf, CONF_sessionname);
    if (!name || !*name || strcmp(name, "Default Settings") == 0)
        return false;

    conf_set_int(term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    err = save_settings(name, term->conf);
    if (err) {
        sfree(err);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Telling the user what is happening
 * ------------------------------------------------------------------------ */

/*
 * Transient tray balloon: add a short-lived notify icon on the session window,
 * fire the balloon, keep the icon alive while it shows, then remove it. Same
 * technique as the file-transfer success notice in kitty_xfer.c, and the reason
 * it is done this way is worth recording: a session window has no permanent tray
 * icon, and adding one just to carry these messages would put an icon in
 * everybody's tray for a feature almost nobody turns on.
 *
 * On Windows 10/11 this renders as a toast and files in the Action Center, so
 * there is a record even if it is missed. Focus Assist can suppress it entirely,
 * which is why the balloon is never the only signal: the Event Log always has the
 * line, and the title marker carries the standing state.
 *
 * The Sleep means this must not run on the GUI thread.
 */
struct osc52_balloon { HWND hwnd; char *title; char *msg; };

static DWORD WINAPI osc52_balloon_thread(LPVOID p)
{
    struct osc52_balloon *b = (struct osc52_balloon *)p;
    static volatile LONG uid = 0xD000;
    NOTIFYICONDATA nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = b->hwnd;
    nid.uID = (UINT)InterlockedIncrement(&uid);
    /* NIF_MESSAGE so that clicking the balloon reaches the terminal window, which
     * opens the Event Log (window.c). Worth the extra field: this balloon is
     * rate-limited, so it can only ever say THAT something was dropped - the
     * Event Log is where how-often and which-one actually live. */
    nid.uFlags = NIF_ICON | NIF_INFO | NIF_MESSAGE;
    nid.uCallbackMessage = WM_KITTY_CLIPBALLOON;
    nid.hIcon = LoadIcon(NULL, IDI_WARNING);
    nid.dwInfoFlags = NIIF_WARNING;
    strncpy(nid.szInfoTitle, b->title, sizeof(nid.szInfoTitle) - 1);
    strncpy(nid.szInfo, b->msg, sizeof(nid.szInfo) - 1);
    if (Shell_NotifyIcon(NIM_ADD, &nid)) {
        Sleep(8000);
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
    sfree(b->title);
    sfree(b->msg);
    sfree(b);
    return 0;
}

/*
 * What clicking the most recent balloon should do. A single value rather than one
 * per balloon because only one is ever on screen at a time - they are rate-limited
 * to one per 30 seconds - and because Windows gives the click back as a bare
 * notification on the window, with no room to carry state of our own.
 *
 * Read by windows/window.c when the click arrives.
 */
static LONG volatile s_balloon_action = CLIP_BALLOON_LOG;

int kitty_clipboard_balloon_action(void)
{
    return (int)InterlockedCompareExchange(&s_balloon_action, 0, 0);
}

/*
 * Is there a title bar to put a marker on?
 *
 * In full screen - and with window decorations switched off - PuTTY drops
 * WS_CAPTION, so both the clipboard icon and the DWM caption tint have nowhere to
 * appear. Everything the window-based signals say is simply invisible in that
 * state, which is exactly when the terminal is filling the screen and the user is
 * least likely to notice anything else either.
 */
bool kitty_osc52_title_visible(void)
{
    if (!MainHwnd)
        return false;
    return (GetWindowLongPtr(MainHwnd, GWL_STYLE) & WS_CAPTION) != 0;
}

void kitty_osc52_notify(Terminal *term, const char *title, const char *msg,
                        int action)
{
    struct osc52_balloon *b;
    HANDLE t;

    if (!MainHwnd || !title || !msg)
        return;
    if (term && term->conf && !conf_get_bool(term->conf, CONF_clipboard_notify))
        return;

    InterlockedExchange(&s_balloon_action, (LONG)action);

    b = snew(struct osc52_balloon);
    b->hwnd = MainHwnd;
    b->title = dupstr(title);
    b->msg = dupstr(msg);
    t = CreateThread(NULL, 0, osc52_balloon_thread, b, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        sfree(b->title);
        sfree(b->msg);
        sfree(b);
    }
}

/* ------------------------------------------------------------------------
 * Colouring the window while a permission is live
 * ------------------------------------------------------------------------ */

/*
 * Windows treats the title bar and the border around the window as two separate
 * things, and both can be set - the frame is worth doing as well as the caption,
 * because the frame is still visible when the window is behind another one and
 * the title bar is partly covered.
 *
 * BOTH need Windows 11 build 22000 or newer. On Windows 10 there is no supported
 * way for an application to colour either, DwmSetWindowAttribute returns a
 * failure for these attribute ids, and nothing happens - so the title marker has
 * to carry the meaning on its own and this is a bonus on top. Resolved at runtime
 * rather than link time so the binary still starts on Windows 10.
 */
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

typedef HRESULT (WINAPI *dwm_set_fn)(HWND, DWORD, LPCVOID, DWORD);

static void osc52_set_frame_colour(HWND hwnd, COLORREF colour)
{
    static dwm_set_fn fn = NULL;
    static bool tried = false;

    if (!tried) {
        HMODULE m = LoadLibraryA("dwmapi.dll");
        if (m)
            fn = (dwm_set_fn)GetProcAddress(m, "DwmSetWindowAttribute");
        tried = true;
    }
    if (!fn || !hwnd)
        return;
    fn(hwnd, DWMWA_CAPTION_COLOR, &colour, sizeof(colour));
    fn(hwnd, DWMWA_BORDER_COLOR, &colour, sizeof(colour));
}

/*
 * A clipboard permission started, expired, or moved between active and paused.
 * Re-apply both signals: the text marker in the title (window.c builds it from
 * term_osc52_perm_state) and the tint.
 */
void kitty_osc52_state_changed(Terminal *term)
{
    int state;
    bool rd, wr;

    /* The marker is part of the decorated title, so re-running the decoration is
     * all that is needed; that path dedupes, so this is cheap when nothing moved. */
    kitty_refresh_title();

    if (!term || !term->conf || !MainHwnd)
        return;
    if (!conf_get_bool(term->conf, CONF_osc52_colour_frame))
        return;

    /*
     * ACTIVITY wins over standing permission, because it is the thing that just
     * happened and it is on screen for only a few seconds.
     *
     * AMBER when the clipboard was READ - data left you for the host. BLUE when it
     * was WRITTEN - the host put something in. Both at once takes amber, the
     * riskier of the two.
     *
     * Deliberately NOT red/green, for two reasons. Risk-wise they come out
     * backwards: the read is the direction that can hand over a password, and the
     * write is the mild one, so "green for copy, red for paste" would paint the
     * dangerous case reassuringly. And red/green is the one pair a large minority
     * of men cannot separate, which for a signal whose entire job is to be read at
     * a glance is the wrong pair to choose. Amber and blue survive both.
     */
    {
        int act = term_clipboard_activity(term);
        if (act) {
            if (act & CLIP_ACT_READ)
                osc52_set_frame_colour(MainHwnd, RGB(224, 160, 48));   /* amber */
            else
                osc52_set_frame_colour(MainHwnd, RGB(72, 140, 224));   /* blue */
            /* and take it back down again when the marker lapses */
            SetTimer(MainHwnd, TIMER_CLIPACTIVITY,
                     (UINT)(conf_get_int(term->conf, CONF_clipboard_activity_secs)
                            > 0 ? conf_get_int(term->conf,
                                               CONF_clipboard_activity_secs) * 1000
                                : 5000), NULL);
            return;
        }
    }

    state = term_osc52_perm_state(term, &rd, &wr);
    switch (state) {
      case OSC52_PERM_ACTIVE:
        /* lilac: unlike a red or yellow, it is not already used by anything else
         * on a Windows desktop, so it reads as "this window is different" rather
         * than as an error */
        osc52_set_frame_colour(MainHwnd, RGB(198, 160, 232));
        break;
      case OSC52_PERM_PAUSED:
        /* muted grey: the permission still exists but is doing nothing, and that
         * is a different fact from both "active" and "none" */
        osc52_set_frame_colour(MainHwnd, RGB(150, 150, 150));
        break;
      case OSC52_PERM_NONE:
      default:
        osc52_set_frame_colour(MainHwnd, (COLORREF)DWMWA_COLOR_DEFAULT);
        break;
    }
}
