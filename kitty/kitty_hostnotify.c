/*
 * kitty_hostnotify.c - desktop notifications from the host (OSC 9, OSC 777,
 * OSC 99), taskbar progress (OSC 9;4) and program status (OSC 7501, with the
 * shell's prompt marks, OSC 133): the Windows half.
 *
 * The parsing, the limits and every decision are in kitty_oscnotify.c and
 * kitty_progstatus.c, which have no Windows in them and are unit-tested
 * (test/test_oscnotify.c, test/test_progstatus.c). What is
 * here needs the window: whether it has the focus or can be seen, the notice
 * near the clock (kitty_notice.c), the flood timer, the replies down the
 * session, and the taskbar button.
 *
 * Notices go to the suite's own notice window, not a tray balloon: a balloon
 * lands in the Action Center, where text a host chose would outlive the
 * session. The notice always carries the session's name as its title, so a
 * host cannot pass its text off as coming from anywhere else.
 *
 * Compiled into the kitty and kitty_portable targets only.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>       /* ITaskbarList3, TBPF_* */

#include "kitty_buildlabel.h"   /* the test hooks below: test builds only */
#include "kitty_commun.h"   /* GetPuttyFlag */
#include "kitty_notice.h"
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include "kitty_osc52.h"    /* kitty_osc52_send_raw: the raw reply seam */
#include "kitty_text.h"
#include "kitty_oscnotify.h"
#include "kitty_progstatus.h"
#include "kitty_hostnotify.h"

extern HWND MainHwnd;          /* kitty.c: the terminal window */

/* The notice's own accent (green and amber belong to the suite's other
 * notices), and red for a critical one (OSC 99 u=2). */
#define HN_ACCENT          RGB(90, 60, 150)
#define HN_ACCENT_CRITICAL RGB(170, 30, 30)

struct hn_shown;

/* Per terminal: the OSC 99 chunk assembly, the flood rule with its pending
 * notice, and the one notice of ours on screen. */
struct kitty_hostnotify {
    Terminal *term;
    OnNotice assembly;
    OnFlood flood;
    bool have_pending;
    OnNotice pending;
    struct hn_shown *shown;
    PsStore ps;                  /* OSC 7501 records */
    int last_exit;               /* OSC 133;D: the last command's exit code */
};

/* What a notice on screen needs when it goes: owned by the notice window
 * (its on_close context) and freed there. owner = NULL once the terminal
 * has gone. */
struct hn_shown {
    struct kitty_hostnotify *owner;
    char id[ON_ID_MAX + 1];
    bool focus, report, close_report;
};

/* ---- test hooks: test builds only, and only when asked for ------------- */

#ifdef KITTY_TEST_BUILD_LABEL
/* KITTY_PROGRESS_TRACE=<file> and KITTY_NOTICE_TRACE=<file>: one line per
 * event, appended. Without the variable nothing is written. */
static void hn_trace(const char *env, const char *line)
{
    const char *path = getenv(env);
    FILE *f;
    if (!path || !*path)
        return;
    f = fopen(path, "a");
    if (!f)
        return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}
#endif

static void hn_trace_notice(COLORREF accent, const char *text)
{
#ifdef KITTY_TEST_BUILD_LABEL
    strbuf *sb;
    const unsigned char *p;
    if (!getenv("KITTY_NOTICE_TRACE"))
        return;
    sb = strbuf_new();
    put_fmt(sb, "notice accent=%06X text=", (unsigned)accent);
    for (p = (const unsigned char *)text; *p; p++) {
        if (*p == '\n')
            put_datapl(sb, PTRLEN_LITERAL("\\n"));
        else if (*p == '\\')
            put_datapl(sb, PTRLEN_LITERAL("\\\\"));
        else if (*p < 0x20)
            put_fmt(sb, "\\x%02X", *p);
        else
            put_byte(sb, *p);
    }
    hn_trace("KITTY_NOTICE_TRACE", sb->s);
    strbuf_free(sb);
#else
    (void)accent;
    (void)text;
#endif
}

static void hn_trace_progress(int state, int value, bool cleared)
{
#ifdef KITTY_TEST_BUILD_LABEL
    char line[64];
    if (cleared)
        snprintf(line, sizeof(line), "progress cleared");
    else
        snprintf(line, sizeof(line), "progress state=%d value=%d", state, value);
    hn_trace("KITTY_PROGRESS_TRACE", line);
#else
    (void)state;
    (void)value;
    (void)cleared;
#endif
}

/* ---- the terminal's state ---------------------------------------------- */

/* The terminal whose program status is on the taskbar button: one window
 * per process, so one at a time. */
static struct kitty_hostnotify *hn_ps_owner;

static struct kitty_hostnotify *hn_get(Terminal *term)
{
    if (!term->hostnotify) {
        term->hostnotify = snew(struct kitty_hostnotify);
        memset(term->hostnotify, 0, sizeof(*term->hostnotify));
        term->hostnotify->term = term;
        term->hostnotify->last_exit = -1;
        hn_ps_owner = term->hostnotify;
    }
    return term->hostnotify;
}

static int hn_setting(Terminal *term)
{
    return term->conf ? conf_get_int(term->conf, CONF_host_notify) : ON_SET_OFF;
}

static bool hn_visible(void)
{
    return MainHwnd && IsWindowVisible(MainHwnd) && !IsIconic(MainHwnd);
}

static bool hn_may_show(Terminal *term, const OnNotice *n)
{
    return on_should_show(hn_setting(term), n->meta.occasion,
                          term->has_focus ? 1 : 0, hn_visible() ? 1 : 0) != 0;
}

static void hn_reply(Terminal *term, const char *buf, size_t len)
{
    if (term && len)
        kitty_osc52_send_raw(term, buf, len);
}

/* ---- the notice -------------------------------------------------------- */

/* The notice window draws ANSI text: UTF-8 in, the ANSI code page out, with
 * '?' for what that code page cannot show. Caller sfrees. */
static char *hn_utf8_to_ansi(const char *utf8)
{
    wchar_t *w = dup_mb_to_wc(CP_UTF8, utf8);
    char *a = dup_wc_to_mb(CP_ACP, w, "?");
    sfree(w);
    return a;
}

/* A notice's title: the session's name - the host name for an unnamed one.
 * Both are held in the ANSI code page (the launcher and the window-title
 * placeholders read them as CP_ACP), which is what the notice draws. */
static const char *hn_session_name(Terminal *term)
{
    const char *sess = conf_get_str(term->conf, CONF_sessionname);
    const char *name = (sess && *sess && strcmp(sess, "Default Settings") != 0) ?
                       sess : conf_get_str(term->conf, CONF_host);
    return (name && *name) ? name : appname;
}

/* Brings the terminal window forward. Allowed from here: the click that got
 * us here was input to this process. A window sent to the tray stays there. */
static void hn_focus_terminal(void)
{
    if (!MainHwnd || !IsWindowVisible(MainHwnd))
        return;
    if (IsIconic(MainHwnd))
        ShowWindow(MainHwnd, SW_RESTORE);
    SetForegroundWindow(MainHwnd);
}

/* The notice is finished with: clicked, timed out, replaced, or closed at
 * the program's request. The click actions first, then the close report. */
static void hn_on_close(void *ctx, int clicked)
{
    struct hn_shown *sh = (struct hn_shown *)ctx;
    struct kitty_hostnotify *st = sh->owner;
    char buf[ON_REPLY_MAX];

    if (st) {
        Terminal *term = st->term;
        if (st->shown == sh)
            st->shown = NULL;
        if (clicked && sh->focus)
            hn_focus_terminal();
        if (clicked && sh->report && sh->id[0])
            hn_reply(term, buf, on_reply_activated(sh->id, buf));
        if (sh->close_report && sh->id[0])
            hn_reply(term, buf, on_reply_closed(sh->id, buf));
    }
    sfree(sh);
}

static void hn_show(struct kitty_hostnotify *st, const OnNotice *n)
{
    Terminal *term = st->term;
    char title[ON_TITLE_MAX + 1], body[ON_BODY_MAX + 1];
    size_t tl, bl;
    char *text, *text_a, *name_a;
    int seconds, critical;
    COLORREF accent;
    struct hn_shown *sh;

    if (!hn_may_show(term, n))
        return;
    /* Already cleaned by hn_offer; cleaning is idempotent. */
    tl = on_text_clean(n->title, n->title_len, title, ON_TITLE_MAX);
    bl = on_text_clean(n->body, n->body_len, body, ON_BODY_MAX);
    if (!tl && !bl)
        return;
    /* The program's title on the first line, then its body. */
    text = tl && bl ? dupcat(title, "\n", body) : dupstr(tl ? title : body);

    seconds = on_seconds(n->meta.urgency, n->meta.expire, &critical);
    accent = critical ? HN_ACCENT_CRITICAL : HN_ACCENT;

    sh = snew(struct hn_shown);
    memset(sh, 0, sizeof(*sh));
    sh->owner = st;
    memcpy(sh->id, n->meta.id, sizeof(sh->id));
    sh->focus = n->meta.focus != 0;
    sh->report = n->meta.report != 0;
    sh->close_report = n->meta.close_report != 0;
    /* Set before the call: the notice it replaces finishes inside it (and
     * must not clear this), and a notice that cannot be shown finishes
     * inside it too (and must) - every failure path of kitty_notice_show_ex
     * runs on_close, which frees sh and clears st->shown. So st->shown is
     * still set afterwards only when this notice is on screen. */
    st->shown = sh;

    text_a = hn_utf8_to_ansi(text);
    name_a = dupstr(hn_session_name(term));   /* titled with the session's name */
    kitty_notice_show_ex(name_a, text_a, accent, seconds, NULL, 0,
                         hn_on_close, sh);
    if (st->shown)
        hn_trace_notice(accent, text_a);
    sfree(name_a);
    sfree(text_a);
    sfree(text);
}

/* The flood timer: the pending notice goes up, or the burst is over. */
static void hn_flood_timer(void *ctx, unsigned long now)
{
    struct kitty_hostnotify *st = (struct kitty_hostnotify *)ctx;
    int dropped;

    if (on_flood_tick(&st->flood, now, &dropped) == ON_FLOOD_NOW) {
        if (st->have_pending) {
            OnNotice n = st->pending;
            st->have_pending = false;
            hn_show(st, &n);
        }
        schedule_timer((int)(on_flood_due(&st->flood) - now), hn_flood_timer, st);
        return;
    }
    if (dropped > 0 && st->term->logctx)
        logeventf(st->term->logctx, KT_HOSTNOTIFY_LOG_DROPPED, dropped);
}

static void hn_offer(struct kitty_hostnotify *st, const OnNotice *in)
{
    unsigned long now;
    OnNotice n = *in;

    /* Cleaned first: a notice with nothing left to show, or one the setting
     * or o= would not show, does not count against the flood rule. */
    n.title_len = on_text_clean(in->title, in->title_len, n.title, ON_TITLE_MAX);
    n.body_len = on_text_clean(in->body, in->body_len, n.body, ON_BODY_MAX);
    if (!n.title_len && !n.body_len)
        return;
    if (!hn_may_show(st->term, &n))
        return;
    now = GETTICKCOUNT();
    if (on_flood_offer(&st->flood, now) == ON_FLOOD_NOW) {
        hn_show(st, &n);
        return;
    }
    st->pending = n;
    st->have_pending = true;
    if (!st->flood.armed) {
        long wait = (long)(on_flood_due(&st->flood) - now);
        st->flood.armed = 1;
        schedule_timer(wait > 0 ? (int)wait : 1, hn_flood_timer, st);
    }
}

/* ---- taskbar progress -------------------------------------------------- */

/* Defined here rather than taken from uuid.lib: CLSID_TaskbarList and
 * IID_ITaskbarList3. */
static const GUID hn_clsid_taskbarlist =
    { 0x56FDF344, 0xFD6D, 0x11d0, { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
static const GUID hn_iid_taskbarlist3 =
    { 0xea1afb91, 0x9e28, 0x4b86, { 0x90, 0xe9, 0x9e, 0x9f, 0x8a, 0x5e, 0xef, 0xaf } };

static ITaskbarList3 *hn_taskbar;
static bool hn_taskbar_tried;      /* created, or failed at hn_taskbar_failed */
static bool hn_taskbar_recorded;   /* the old-Windows report, once */
static DWORD hn_taskbar_failed;    /* tick of the last failed attempt */
static int hn_prog_state;          /* the OSC 9;4 state last applied */
static int hn_prog_value;

/* A failed creation is tried again after this long, not never: the shell
 * may simply not have been up yet. */
#define HN_TASKBAR_RETRY_MS 60000

static void hn_taskbar_release(void)
{
    if (hn_taskbar)
        hn_taskbar->lpVtbl->Release(hn_taskbar);
    hn_taskbar = NULL;
    hn_taskbar_tried = false;
}

/* The taskbar interface, created on first use. COM, so nothing is imported
 * that Windows before 7 lacks: there the object simply does not exist and
 * progress stays off. */
static ITaskbarList3 *hn_taskbar_get(void)
{
    HRESULT hr;
    if (hn_taskbar_tried &&
        (hn_taskbar || GetTickCount() - hn_taskbar_failed < HN_TASKBAR_RETRY_MS))
        return hn_taskbar;
    hn_taskbar_tried = true;
    hn_taskbar = NULL;
    hr = CoCreateInstance(&hn_clsid_taskbarlist, NULL, CLSCTX_INPROC_SERVER,
                          &hn_iid_taskbarlist3, (void **)&hn_taskbar);
    if (hr == CO_E_NOTINITIALIZED) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        hr = CoCreateInstance(&hn_clsid_taskbarlist, NULL, CLSCTX_INPROC_SERVER,
                              &hn_iid_taskbarlist3, (void **)&hn_taskbar);
    }
    if (SUCCEEDED(hr) && hn_taskbar &&
        FAILED(hn_taskbar->lpVtbl->HrInit(hn_taskbar))) {
        hn_taskbar->lpVtbl->Release(hn_taskbar);
        hn_taskbar = NULL;
    }
    if (FAILED(hr))
        hn_taskbar = NULL;
    if (!hn_taskbar)
        hn_taskbar_failed = GetTickCount();
    if (!hn_taskbar_recorded) {
        hn_taskbar_recorded = true;
        kitty_api_record("shell32.dll", "ITaskbarList3", KITTY_API_OPTIONAL,
                         KT_WINFEAT_TASKBAR_PROGRESS, hn_taskbar != NULL);
    }
    return hn_taskbar;
}

static void hn_progress_apply(int state, int value)
{
    ITaskbarList3 *tb = hn_taskbar_get();
    TBPFLAG flag = state == 1 ? TBPF_NORMAL :
                   state == 2 ? TBPF_ERROR :
                   state == 3 ? TBPF_INDETERMINATE :
                   state == 4 ? TBPF_PAUSED : TBPF_NOPROGRESS;
    if (tb && MainHwnd) {
        tb->lpVtbl->SetProgressState(tb, MainHwnd, flag);
        if (state == 1 || state == 2 || state == 4)
            tb->lpVtbl->SetProgressValue(tb, MainHwnd, (ULONGLONG)value, 100);
    }
}

static void hn_progress(int state, int value)
{
    /* No value given: keep the last one (an error or pause marks the bar
     * where it stands). */
    if (value < 0)
        value = (state == 1 || state == 2 || state == 4) ? hn_prog_value : 0;
    hn_progress_apply(state, value);
    hn_prog_state = state;
    hn_prog_value = state == 0 ? 0 : value;
    hn_trace_progress(state, value, false);
}

static void hn_ps_session_ended(void);

void kitty_hostnotify_session_ended(void)
{
    hn_ps_session_ended();
    if (hn_prog_state == 0)
        return;
    hn_progress_apply(0, 0);
    hn_prog_state = 0;
    hn_prog_value = 0;
    hn_trace_progress(0, 0, true);
}

static void hn_ps_icons_free(void);
static void hn_ps_overlay_again(void);

void kitty_hostnotify_shutdown(void)
{
    hn_taskbar_release();
    hn_ps_icons_free();
}

void kitty_hostnotify_taskbar_message(unsigned int msg)
{
    static UINT created = 0;
    if (!created)
        created = RegisterWindowMessageA("TaskbarButtonCreated");
    if (!created || msg != created)
        return;
    /* A new button, possibly a new shell: the old interface may be dead. */
    hn_taskbar_release();
    if (hn_prog_state != 0)
        hn_progress_apply(hn_prog_state, hn_prog_value);
    hn_ps_overlay_again();
}

/* ---- program status (OSC 7501) and prompt marks (OSC 133) -------------- */

/* What the taskbar button shows for it: the overlay applied (PS_* of the
 * summary; PS_IDLE = none) and the bar it drives (OSC 9;4 states). The bar is
 * shared with OSC 9;4: it is set only when the summary's bar changes, so the
 * last of the two to report wins. */
static int hn_ps_overlay = PS_IDLE;
static int hn_ps_overlay_kind;
static int hn_ps_bar_state, hn_ps_bar_value;
static HICON hn_ps_icons[PS_BLOCKED + 1];

/* The words for a state, on the overlay (read by screen readers) and in the
 * notice. */
static const char *hn_ps_phrase(int state, int kind)
{
    switch (state) {
      case PS_WORKING: return KT_PST_WORKING;
      case PS_DONE:    return KT_PST_DONE;
      case PS_ERROR:   return KT_PST_ERROR;
      case PS_BLOCKED:
        return kind == PS_KIND_PERMISSION ? KT_PST_BLOCKED_PERMISSION :
               kind == PS_KIND_QUESTION   ? KT_PST_BLOCKED_QUESTION :
               kind == PS_KIND_AUTH       ? KT_PST_BLOCKED_AUTH :
                                            KT_PST_BLOCKED;
      default:         return "";
    }
}

/* The overlay: a dot in the state's colour with a white rim, drawn once at
 * the small-icon size. Straight alpha, so the rim is smooth on any taskbar. */
static HICON hn_ps_icon(int state)
{
    COLORREF col;
    int sz, x, y;
    size_t masklen;
    BITMAPINFO bi;
    DWORD *px = NULL;
    HBITMAP color, mask;
    ICONINFO ii;
    unsigned char *zero;
    double c, r;

    if (state < 0 || state > PS_BLOCKED)
        return NULL;
    if (hn_ps_icons[state])
        return hn_ps_icons[state];
    col = state == PS_BLOCKED ? RGB(232, 152, 0) :
          state == PS_ERROR   ? RGB(204, 36, 36) :
          state == PS_WORKING ? RGB(40, 112, 204) : RGB(36, 148, 64);
    sz = GetSystemMetrics(SM_CXSMICON);
    if (sz < 8)
        sz = 16;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!color || !px)
        return NULL;
    c = (sz - 1) / 2.0;
    r = sz / 2.0 - 0.5;
    for (y = 0; y < sz; y++) {
        for (x = 0; x < sz; x++) {
            double d = sqrt((x - c) * (x - c) + (y - c) * (y - c));
            double a = r + 0.5 - d;          /* coverage of the whole dot */
            double in = r - 1.0 - d;         /* coverage of the coloured part */
            int rr, gg, bb, aa;
            if (a > 1) a = 1;
            if (a < 0) a = 0;
            if (in > 1) in = 1;
            if (in < 0) in = 0;
            rr = (int)(GetRValue(col) * in + 255 * (1 - in));
            gg = (int)(GetGValue(col) * in + 255 * (1 - in));
            bb = (int)(GetBValue(col) * in + 255 * (1 - in));
            aa = (int)(a * 255 + 0.5);
            px[y * sz + x] = ((DWORD)aa << 24) | ((DWORD)rr << 16) |
                             ((DWORD)gg << 8) | (DWORD)bb;
        }
    }
    masklen = (size_t)((sz + 15) / 16 * 2) * (size_t)sz;
    zero = snewn(masklen, unsigned char);
    memset(zero, 0, masklen);
    mask = CreateBitmap(sz, sz, 1, 1, zero);
    sfree(zero);
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    hn_ps_icons[state] = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask)
        DeleteObject(mask);
    return hn_ps_icons[state];
}

static void hn_ps_icons_free(void)
{
    int i;
    for (i = 0; i <= PS_BLOCKED; i++) {
        if (hn_ps_icons[i])
            DestroyIcon(hn_ps_icons[i]);
        hn_ps_icons[i] = NULL;
    }
}

static void hn_ps_overlay_apply(int state, int kind)
{
    ITaskbarList3 *tb = hn_taskbar_get();
    wchar_t *desc;
    if (!tb || !MainHwnd)
        return;
    if (state == PS_IDLE) {
        tb->lpVtbl->SetOverlayIcon(tb, MainHwnd, NULL, NULL);
        return;
    }
    desc = dup_mb_to_wc(CP_ACP, hn_ps_phrase(state, kind));
    tb->lpVtbl->SetOverlayIcon(tb, MainHwnd, hn_ps_icon(state), desc);
    sfree(desc);
}

static void hn_ps_overlay_again(void)
{
    if (hn_ps_overlay != PS_IDLE)
        hn_ps_overlay_apply(hn_ps_overlay, hn_ps_overlay_kind);
}

static void hn_trace_status(void)
{
#ifdef KITTY_TEST_BUILD_LABEL
    char line[96];
    snprintf(line, sizeof(line), "status overlay=%d kind=%d bar=%d value=%d",
             hn_ps_overlay, hn_ps_overlay_kind, hn_ps_bar_state,
             hn_ps_bar_value);
    hn_trace("KITTY_PROGRESS_TRACE", line);
#endif
}

/* The taskbar button follows the records' summary. */
static void hn_ps_refresh(struct kitty_hostnotify *st)
{
    Terminal *term = st->term;
    bool on = term->conf && conf_get_bool(term->conf, CONF_taskbar_progress);
    PsView v;
    int ov, bar = 0, val = 0;

    ps_view(&st->ps, &v);
    ov = on ? v.state : PS_IDLE;
    if (ov == PS_WORKING) {
        bar = v.progress >= 0 ? 1 : 3;           /* normal / busy */
        val = v.progress >= 0 ? v.progress : 0;
    } else if (ov == PS_BLOCKED) {
        bar = 4;                                 /* paused: yellow */
        val = v.progress >= 0 ? v.progress : 100;
    }
    if (ov != hn_ps_overlay ||
        (ov == PS_BLOCKED && v.kind != hn_ps_overlay_kind)) {
        /* Newly blocked while the window is in the background: the button
         * flashes until the window comes forward. */
        if (ov == PS_BLOCKED && hn_ps_overlay != PS_BLOCKED &&
            !term->has_focus && MainHwnd) {
            FLASHWINFO fi;
            memset(&fi, 0, sizeof(fi));
            fi.cbSize = sizeof(fi);
            fi.hwnd = MainHwnd;
            fi.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
            FlashWindowEx(&fi);
        }
        hn_ps_overlay_apply(ov, v.kind);
        hn_ps_overlay = ov;
        hn_ps_overlay_kind = v.kind;
    }
    if (bar != hn_ps_bar_state || val != hn_ps_bar_value) {
        if (bar != 0 || hn_ps_bar_state != 0)
            hn_progress(bar, val);
        hn_ps_bar_state = bar;
        hn_ps_bar_value = val;
    }
    hn_trace_status();
}

/* A record became blocked, done or failed: a notice, under the same setting,
 * limits and flood rule as the host's own notices. "<name>: <state>" on the
 * first line, the program's message below. */
static void hn_ps_notice(struct kitty_hostnotify *st, const PsRecord *r)
{
    OnNotice n;
    const char *name = r->title[0] ? r->title : ps_app_of(&st->ps, r);
    const char *phrase = hn_ps_phrase(r->state, r->kind);
    char *first = name[0] ? dupprintf(KT_PST_NOTICE_FMT, name, phrase) :
                            dupstr(phrase);
    on_notice_set(&n, first, strlen(first), r->msg, strlen(r->msg));
    sfree(first);
    hn_offer(st, &n);
}

static void hn_ps_osc7501(Terminal *term, const char *s, size_t len,
                          bool overflow)
{
    struct kitty_hostnotify *st;
    PsRecord r;
    PsEntry *e;
    int prev;

    if (overflow)
        return;                          /* over the limit: discarded whole */
    switch (ps_parse(s, len, &r)) {
      case PS_QUERY:
        /* The one reply: the fixed body. Nothing of a report goes back. */
        if (hn_setting(term) != ON_SET_OFF ||
            conf_get_bool(term->conf, CONF_taskbar_progress))
            hn_reply(term, PS_QUERY_REPLY, strlen(PS_QUERY_REPLY));
        return;
      case PS_REPORT:
        break;
      default:
        return;
    }
    st = hn_get(term);
    e = ps_apply(&st->ps, &r, &prev);
    if (e && (e->r.state == PS_DONE || e->r.state == PS_ERROR) &&
        term->has_focus)
        e->seen = 1;                     /* seen as it happened */
    if (e && e->r.state != prev &&
        (e->r.state == PS_BLOCKED || e->r.state == PS_DONE ||
         e->r.state == PS_ERROR) && hn_setting(term) != ON_SET_OFF)
        hn_ps_notice(st, &e->r);
    hn_ps_refresh(st);
}

static void hn_ps_osc133(Terminal *term, const char *s, size_t len)
{
    struct kitty_hostnotify *st;
    int code;
    switch (ps_osc133(s, len, &code)) {
      case PS133_PROMPT:
        /* A prompt: whatever was running has ended. */
        if (!term->hostnotify)
            return;
        st = term->hostnotify;
        ps_drop_running(&st->ps);
        hn_ps_refresh(st);
        break;
      case PS133_END:
        hn_get(term)->last_exit = code;
        break;
      default:
        break;
    }
}

/* The session ended: nothing on the host runs any more. */
static void hn_ps_session_ended(void)
{
    if (!hn_ps_owner)
        return;
    ps_drop_running(&hn_ps_owner->ps);
    hn_ps_refresh(hn_ps_owner);
}

void kitty_hostnotify_focus(Terminal *term)
{
    if (!term || !term->hostnotify)
        return;
    ps_mark_seen(&term->hostnotify->ps);
    hn_ps_refresh(term->hostnotify);
}

/* ---- entry points ------------------------------------------------------ */

/* OSC 9 and 777 come in the session's line code page; OSC 99 is UTF-8 by
 * its specification. Returns a UTF-8 copy (caller sfrees) and its length. */
static char *hn_to_utf8(Terminal *term, const char *s, size_t len, size_t *outlen)
{
    int cp = term->ucsdata ? term->ucsdata->line_codepage : CP_UTF8;
    wchar_t *w;
    char *u;
    size_t wlen;
    if (cp == CP_UTF8) {
        u = snewn(len + 1, char);
        memcpy(u, s, len);
        u[len] = '\0';
        *outlen = len;
        return u;
    }
    w = dup_mb_to_wc_c(cp, s, len, &wlen);
    u = dup_wc_to_mb_c(CP_UTF8, w, wlen, "?", outlen);
    sfree(w);
    return u;
}

void kitty_hostnotify_osc(Terminal *term, unsigned osc, const char *s,
                          size_t len, bool overflow)
{
    struct kitty_hostnotify *st;
    OnNotice n;
    char buf[ON_REPLY_MAX];

    if (!term || !term->conf || GetPuttyFlag())
        return;

    if (osc == 7501) {
        hn_ps_osc7501(term, s, len, overflow);
        return;
    }
    if (osc == 133) {
        if (!overflow)
            hn_ps_osc133(term, s, len);
        return;
    }

    if (osc == 9) {
        int state, value;
        int kind = on_osc9(s, len, &state, &value);
        if (kind == ON9_PROGRESS) {
            if (conf_get_bool(term->conf, CONF_taskbar_progress))
                hn_progress(state, value);
            return;
        }
        if (kind != ON9_NOTICE || hn_setting(term) == ON_SET_OFF)
            return;
        {
            size_t ulen;
            char *u = hn_to_utf8(term, s, len, &ulen);
            on_notice_set(&n, NULL, 0, u, ulen);
            sfree(u);
        }
        hn_offer(hn_get(term), &n);
        return;
    }

    if (hn_setting(term) == ON_SET_OFF)
        return;                          /* not even the queries are answered */

    if (osc == 777) {
        size_t ulen;
        char *u = hn_to_utf8(term, s, len, &ulen);
        int ok = on_osc777(u, ulen, &n);
        sfree(u);
        if (ok)
            hn_offer(hn_get(term), &n);
        return;
    }

    if (osc != 99 || overflow)
        return;                          /* a cut OSC 99 is not acted on */
    st = hn_get(term);
    switch (on_osc99(&st->assembly, s, len, &n)) {
      case ON99_SHOW:
        hn_offer(st, &n);
        break;
      case ON99_CLOSE:
        /* Ours and current: closed (its close report goes as usual). A
         * pending one of that id never shows. */
        if (st->have_pending && !strcmp(st->pending.meta.id, n.meta.id)) {
            st->have_pending = false;
            st->flood.pending = 0;   /* the timer then just ends the burst */
        }
        if (st->shown && n.meta.id[0] && !strcmp(st->shown->id, n.meta.id) &&
            kitty_notice_showing(st->shown))
            kitty_notice_close_ctx(st->shown);
        break;
      case ON99_ALIVE: {
        const char *alive = (st->shown && st->shown->id[0] &&
                             kitty_notice_showing(st->shown)) ?
                            st->shown->id : "";
        hn_reply(term, buf, on_reply_alive(n.meta.id, alive, buf));
        break;
      }
      case ON99_QUERY:
        hn_reply(term, buf, on_reply_query(n.meta.id, buf));
        break;
      default:
        break;
    }
}

void kitty_host_notice(Terminal *term, const char *title, const char *body)
{
    OnNotice n;
    if (!term || !term->conf || GetPuttyFlag() || hn_setting(term) == ON_SET_OFF)
        return;
    on_notice_set(&n, title, title ? strlen(title) : 0,
                  body, body ? strlen(body) : 0);
    hn_offer(hn_get(term), &n);
}

/*
 * KiTTY far2l: the key events turned on or off and the state held for a
 * moment (window.c debounces). KiTTY++'s own notice, not the host's: shown
 * whatever the HostNotify setting, the focus or the flood rule, and in PuTTY
 * mode too, where the keys go to far2l as well. Titled like the host's
 * notices; the suite's green. Each notice gets a context of its own, so the
 * on_close of the notice it replaces (run inside the call) cannot be taken
 * for its own failure.
 */
#define HN_ACCENT_KEYS RGB(0, 100, 0)

static void *hn_keys_shown;

static void hn_keys_on_close(void *ctx, int clicked)
{
    (void)clicked;
    if (hn_keys_shown == ctx)
        hn_keys_shown = NULL;
}

void kitty_far2l_keys_notice(Terminal *term, bool on)
{
    static uintptr_t serial;
    const char *text = on ? KT_FAR2L_KEYS_NOTICE_ON : KT_FAR2L_KEYS_NOTICE_OFF;
    void *ctx;

    if (!term || !term->conf)
        return;
    if (!++serial)
        serial = 1;
    ctx = (void *)serial;
    hn_keys_shown = ctx;
    kitty_notice_show_ex(hn_session_name(term), text, HN_ACCENT_KEYS, 10,
                         NULL, 0, hn_keys_on_close, ctx);
    if (hn_keys_shown == ctx)
        hn_trace_notice(HN_ACCENT_KEYS, text);
}

void kitty_hostnotify_term_free(Terminal *term)
{
    struct kitty_hostnotify *st;
    if (!term || !term->hostnotify)
        return;
    st = term->hostnotify;
    expire_timer_context(st);
    if (st->shown)
        st->shown->owner = NULL;     /* the notice may outlive us */
    ps_free(&st->ps);
    if (hn_ps_owner == st)
        hn_ps_owner = NULL;
    smemclr(st, sizeof(*st));
    sfree(st);
    term->hostnotify = NULL;
}
