/*
 * kitty_transfer.c - kitty's file-transfer protocol (OSC 5113): the terminal
 * half, so that `kitten transfer` on the far end can move files through the
 * terminal byte stream itself - across nested ssh hops, telnet, a serial line.
 *
 * Spec: docs/file-transfer-protocol.rst in the kitty repository. The wire
 * parser and the name rules are in kitty_transfer.h (shared with the unit
 * test); this file holds the sessions, the Win32 file work, the permission
 * dialog and the replies. This is the in-terminal protocol; the unrelated
 * helper-program transfers (kscp, WinSCP, FileZilla) are one letter away in
 * kitty_xfer.c.
 *
 * Two kinds of session, one of each at a time per terminal:
 *
 *   send    (far end -> this PC)  send / [dialog] / file... / data... /
 *                                 end_data / finish
 *   receive (this PC -> far end)  receive+specs / [dialog] / listing /
 *                                 file requests / data... / finished
 *
 * Both dialogs are MODELESS: the terminal keeps running while the question
 * stands, and the answer continues the session from a completion
 * (kt_send_decide / kt_recv_decide) rather than from where it was asked.
 *
 * Every reply goes out through kitty_osc52_send_raw(), straight to the
 * backend and never through the line editor (see that function for why).
 *
 * What is refused on purpose: the bypass password (a dialog is always the
 * gate), links of either kind, rsync deltas (a STARTED without tt= makes the
 * client fall back to plain data; a receive-side rsync request gets EINVAL),
 * and any name that could leave the destination folder. Compression: inbound
 * zlib is inflated with PuTTY's own RFC 1950 decoder; outbound "zlib" is a
 * valid stream of stored blocks - PuTTY's compressor cannot close a stream,
 * so no actual compression is done in that direction.
 *
 * Compiled into the kitty and kitty_portable targets only. terminal.c calls
 * the two entry points through weak references.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "terminal.h"
#include "ssh.h"                 /* ssh_zlib: the RFC 1950 inflater */

#include <windows.h>
#include <commctrl.h>            /* the upload-request dialog's list view */

#include "kitty.h"               /* kitty_xfer_download_dir, kitty_xfer_upload_dir */
#include "kitty_winutil.h"       /* OpenDirNameFrom, OpenFileNameFrom */
#include "kitty_dlgbox.h"        /* the suite's themed Yes/No box */
#include "kitty_theme.h"         /* the shared painter: ink marks, button widths */
#include "kitty_anchor.h"        /* the shared resize: edge anchoring */
#include "kitty_text.h"          /* KT_XFER_WHAT_KITTEN_* for the notification */
#include "kitty_inikeys.h"       /* KI_*: the kitty.ini key names */
#include "kitty_transfer_text.h" /* kitty.h brings kitty_rc_additions.h: IDD_XFERREQ, IDD_XFERDL */
#define KT5113_PARSE_IMPL
#include "kitty_transfer.h"
#include "kitty_osc52.h"

extern HWND MainHwnd;            /* kitty.c: the terminal window */
/* The reply channel (kitty_osc52.c): a complete sequence to the backend. */

#define KT_EXPIRE_SECONDS   (10 * 60)   /* idle session, as the reference does */
#define KT_MAX_FILES        8192        /* per session, either direction */
#define KT_CHUNK            4096        /* the spec's chunk size */
#define KT_CHUNK_ZIP        4000        /* leaves room for the stored-block framing */
#define KT_SENDBUF_HIGH     (256 * 1024)/* stop pumping while this much is queued */
#define KT_PUMP_TICKS       (TICKSPERSEC / 100)
#define KT_MAX_DEPTH        32          /* directory recursion when listing */
#define KT_DIALOG_LINES     6           /* paths shown in the read dialog */

/* ------------------------------------------------------------------------
 * Session state
 * ------------------------------------------------------------------------ */

/* A file the far end is sending. Refused metadata still gets an entry, so the
 * data that follows can be discarded by file_id as the spec requires. */
struct kt_file {
    char fid[KT5113_ID_MAX + 1];
    int ftype;
    int refused, done, failed;
    wchar_t *final_path, *part_path;
    char *posix;                        /* final path in /C:/... form, for n= */
    HANDLE h;
    uint64_t written;
    uint64_t ceiling;                   /* the session's per-file limit, 0 = none */
    int64_t mtime;                      /* ns since the epoch, -1 = none */
    int readonly;
    ssh_decompressor *dec;              /* zlib inbound, else NULL */
    unsigned char tail[4];              /* the last four bytes seen: the Adler-32 */
    int tail_len;
    uint32_t adler;                     /* of the inflated data */
    struct kt_file *next;
};

/* A directory the far end declared, so files inside it keep their place. */
struct kt_dir {
    char *name;                         /* POSIX, no trailing slash */
    size_t name_len;
    wchar_t *local;
    struct kt_dir *next;
};

struct kt_send {
    char id[KT5113_ID_MAX + 1];
    int quiet;
    int accepted;
    int in_dialog, cancel_pending, abort_pending;
    HWND dlg;                           /* the request dialog while it stands */
    wchar_t *dest;
    uint64_t ceiling;                   /* per-file byte limit, 0 = none */
    time_t last;
    int nfiles, ndone;
    struct kt_file *files;
    struct kt_dir *dirs;
};

/* One entry of the listing sent to the far end in a receive session. Data
 * requests are honoured only for names in this list. Built by the walk
 * BEFORE the dialog (the dialog lists the files), replied after it; a file
 * left unchecked in the dialog stays listed and is refused when asked for. */
struct kt_entry {
    char rid[24];                       /* our id for it: the st= of the listing */
    char spec_fid[KT5113_ID_MAX + 1];   /* the request it answers */
    char *posix;                        /* the name sent to the far end */
    wchar_t *local;                     /* NULL while `missing` */
    int is_dir;
    uint64_t size;
    int64_t mtime_ns;
    int readonly;
    int denied;                         /* unchecked in the dialog */
    /* A requested name with no file behind it. Listed in the dialog so the
     * request is visible, never listed to the far end, and refused as its
     * spec's ENOENT - unless "Locate..." puts a local file behind it, which
     * clears this and leaves `posix` as the name that was ASKED for. */
    int missing;
    struct kt_entry *parent;            /* the folder entry it sits in, or NULL */
    struct kt_entry *next;
};

struct kt_spec {
    char fid[KT5113_ID_MAX + 1];
    char *name;
    const char *why, *code;             /* refused by the walk: sent after OK */
};

/* A queued data request: one file, streamed in chunks by the pump. */
struct kt_req {
    char fid[KT5113_ID_MAX + 1];
    struct kt_entry *e;
    int zip;
    HANDLE h;
    uint64_t size, sent;
    uint32_t adler;
    int header_sent;
    struct kt_req *next;
};

struct kt_recv {
    char id[KT5113_ID_MAX + 1];
    int quiet;
    int accepted, listed;
    int in_dialog, cancel_pending, abort_pending;
    HWND dlg;                           /* the request dialog while it stands */
    int nspecs, got;
    struct kt_spec *specs;
    struct kt_entry *entries;
    int nentries, nfiles;               /* nfiles: entries that are not folders */
    uint64_t total_bytes;               /* of those files, for the dialog */
    uint64_t rid_counter;
    struct kt_req *queue;
    int timer_armed;
    time_t last;
    int nsent;
};

/* Per-terminal state. Kept outside the Terminal struct (terminal.h is the
 * cross-platform file) in a small list keyed by the pointer. The request
 * dialogs are modeless and outlive the call that raised them, so a node is
 * marked `dead` when the terminal goes and is dropped by whichever of the two
 * finishes last - see kt_state_maybe_drop and kitty_transfer_free. */
struct kt_state {
    Terminal *term;
    struct kt_send *send;
    struct kt_recv *recv;
    int latched;                        /* policy 1: a grant was given */
    int oversize_logged;
    int dead;                           /* the terminal went away */
    int freeing;                        /* inside kitty_transfer_free */
    struct kt_state *next;
};

static struct kt_state *kt_states = NULL;

static struct kt_state *kt_state_get(Terminal *term, int create)
{
    struct kt_state *st;
    for (st = kt_states; st; st = st->next)
        if (st->term == term && !st->dead)
            return st;
    if (!create)
        return NULL;
    st = snew(struct kt_state);
    memset(st, 0, sizeof(*st));
    st->term = term;
    st->next = kt_states;
    kt_states = st;
    return st;
}

static void kt_state_drop(struct kt_state *st)
{
    struct kt_state **pp;
    for (pp = &kt_states; *pp; pp = &(*pp)->next)
        if (*pp == st) {
            *pp = st->next;
            sfree(st);
            return;
        }
}

/*
 * A completion that has just finished the last half of a dead terminal's
 * state drops it. Never while kitty_transfer_free() is on the stack: that
 * function destroys the dialogs itself and does the dropping afterwards, and
 * a node freed underneath it would be read again on the way out.
 */
static void kt_state_maybe_drop(struct kt_state *st)
{
    if (!st->dead || st->freeing || st->send || st->recv)
        return;
    kt_state_drop(st);
}

/* ------------------------------------------------------------------------
 * Small helpers: strings, paths, logging
 * ------------------------------------------------------------------------ */

static void kt_log(Terminal *term, char *msg)
{
    logevent(term->logctx, msg);
    sfree(msg);
}

/* The two limits with a session value and a global default (Connection >
 * Transfers, else KiTTY++ Settings > Transfers & Tools > OSC 5113 (kitten)):
 * a session value below 0 means "the global one". */
static int kt_global_int(const char *key, int dflt)
{
    char v[64] = "";
    if (ReadParameterN(INIT_SECTION, key, v, sizeof(v)) && v[0]) {
        if (!stricmp(v, "yes")) return 1;
        if (!stricmp(v, "no")) return 0;
        return atoi(v);
    }
    return dflt;
}

/* "Max transfer size (MB)" as a byte count; 0 = no limit. */
static uint64_t kt_max_bytes(Conf *conf)
{
    int mb = conf_get_int(conf, CONF_xfer_max_mb);
    if (mb < 0)
        mb = kt_global_int(KI_TRANSFERMAXMB, 1024);
    if (mb <= 0)
        return 0;
    return (uint64_t)mb << 20;
}

/* "Allow full path Upload-Requests": may a /C:/... spec name a file? */
static int kt_full_path_allowed(Conf *conf)
{
    int v = conf_get_int(conf, CONF_xfer_full_path);
    if (v < 0)
        v = kt_global_int(KI_TRANSFERFULLPATH, 0);
    return v != 0;
}

static wchar_t *kt_mb_to_wide(int cp, const char *s, int n)
{
    int cnt = MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                                  s, n, NULL, 0);
    wchar_t *w;
    if (cnt <= 0)
        return NULL;
    w = snewn((size_t)cnt + 1, wchar_t);
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s, n, w, cnt);
    w[cnt] = L'\0';
    return w;
}

static char *kt_wide_to_mb(int cp, const wchar_t *w)
{
    int cnt = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (cnt <= 0)
        return dupstr("");
    s = snewn((size_t)cnt, char);
    WideCharToMultiByte(cp, 0, w, -1, s, cnt, NULL, NULL);
    return s;
}

/* For the Event Log and the dialog, which are ANSI. */
static char *kt_ansi(const wchar_t *w)
{
    return kt_wide_to_mb(CP_ACP, w);
}

/* A local path in the spec's form: /C:/Users/x/file, UTF-8. */
static char *kt_posix(const wchar_t *w)
{
    char *u = kt_wide_to_mb(CP_UTF8, w), *p, *out;
    for (p = u; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (u[0] && u[1] == ':')
        out = dupprintf("/%s", u);
    else
        out = dupstr(u);
    sfree(u);
    return out;
}

static wchar_t *kt_wjoin(const wchar_t *dir, const wchar_t *name)
{
    size_t dl = wcslen(dir), nl = wcslen(name);
    int sep = dl > 0 && dir[dl - 1] != L'\\' && dir[dl - 1] != L'/';
    wchar_t *out = snewn(dl + nl + 2, wchar_t);
    memcpy(out, dir, dl * sizeof(wchar_t));
    if (sep)
        out[dl++] = L'\\';
    memcpy(out + dl, name, (nl + 1) * sizeof(wchar_t));
    return out;
}

static wchar_t *kt_wdup(const wchar_t *w)
{
    size_t n = wcslen(w) + 1;
    wchar_t *out = snewn(n, wchar_t);
    memcpy(out, w, n * sizeof(wchar_t));
    return out;
}

static DWORD kt_attrs(const wchar_t *p)
{
    return GetFileAttributesW(p);
}

static int kt_exists(const wchar_t *p)
{
    return kt_attrs(p) != INVALID_FILE_ATTRIBUTES;
}

static int kt_is_dir(const wchar_t *p)
{
    DWORD a = kt_attrs(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *kt_part_name(const wchar_t *final_path)
{
    size_t n = wcslen(final_path);
    wchar_t *p = snewn(n + 6, wchar_t);
    memcpy(p, final_path, n * sizeof(wchar_t));
    memcpy(p + n, L".part", 6 * sizeof(wchar_t));
    return p;
}

/* A name in `dir` that exists neither as itself nor as its .part: the wanted
 * name, else "name (2).ext", "name (3).ext" ... Nothing is ever overwritten. */
static wchar_t *kt_unique(const wchar_t *wanted)
{
    const wchar_t *dot, *slash;
    size_t stem;
    int n;

    {
        wchar_t *part = kt_part_name(wanted);
        int free_ = !kt_exists(wanted) && !kt_exists(part);
        sfree(part);
        if (free_)
            return kt_wdup(wanted);
    }
    slash = wcsrchr(wanted, L'\\');
    dot = wcsrchr(wanted, L'.');
    if (!dot || (slash && dot < slash) || dot == (slash ? slash + 1 : wanted))
        dot = wanted + wcslen(wanted);    /* no extension to keep */
    stem = (size_t)(dot - wanted);
    for (n = 2; n < 10000; n++) {
        wchar_t *cand = snewn(wcslen(wanted) + 16, wchar_t), *part;
        int free_;
        memcpy(cand, wanted, stem * sizeof(wchar_t));
        swprintf(cand + stem, 16 + wcslen(dot), L" (%d)%ls", n, dot);
        part = kt_part_name(cand);
        free_ = !kt_exists(cand) && !kt_exists(part);
        sfree(part);
        if (free_)
            return cand;
        sfree(cand);
    }
    return NULL;
}

static int64_t kt_filetime_to_ns(const FILETIME *ft)
{
    uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (v < 116444736000000000ULL)
        return 0;
    return (int64_t)((v - 116444736000000000ULL) * 100ULL);
}

static void kt_ns_to_filetime(int64_t ns, FILETIME *ft)
{
    uint64_t v = (uint64_t)(ns / 100) + 116444736000000000ULL;
    ft->dwLowDateTime = (DWORD)(v & 0xffffffffu);
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

/* ------------------------------------------------------------------------
 * Replies
 * ------------------------------------------------------------------------ */

static void kt_put_b64(strbuf *sb, const char *key, const void *data, size_t n)
{
    char *b = snewn(kt5113_b64_size(n), char);
    kt5113_b64_encode((const unsigned char *)data, n, b);
    put_fmt(sb, ";%s=%s", key, b);
    sfree(b);
}

static void kt_send_seq(Terminal *term, strbuf *sb)
{
    put_data(sb, "\033\\", 2);
    kitty_osc52_send_raw(term, sb->s, sb->len);
    strbuf_free(sb);
}

/* action=status. `msg` (may be NULL) follows the code after a colon, as the
 * spec's examples do. `name` is a POSIX path or NULL; `size` < 0 is omitted.
 * quiet=1 drops acknowledgements, quiet=2 drops errors as well. */
static void kt_status(Terminal *term, int quiet, int is_error, const char *id,
                      const char *fid, const char *code, const char *msg,
                      const char *name, int64_t size)
{
    strbuf *sb;
    char st[KT5113_ST_MAX + 64];

    if (quiet >= 2 || (quiet >= 1 && !is_error))
        return;
    sb = strbuf_new();
    put_fmt(sb, "\033]5113;ac=status;id=%s", id);
    if (fid && *fid)
        put_fmt(sb, ";fid=%s", fid);
    if (msg && *msg)
        snprintf(st, sizeof(st), "%s:%s", code, msg);
    else
        snprintf(st, sizeof(st), "%s", code);
    kt_put_b64(sb, "st", st, strlen(st));
    if (name)
        kt_put_b64(sb, "n", name, strlen(name));
    if (size >= 0)
        put_fmt(sb, ";sz=%lld", (long long)size);
    kt_send_seq(term, sb);
}

#define kt_ack(term, s, fid, code, name, size) \
    kt_status(term, (s)->quiet, 0, (s)->id, fid, code, NULL, name, size)
#define kt_err(term, s, fid, code, msg) \
    kt_status(term, (s)->quiet, 1, (s)->id, fid, code, msg, NULL, -1)

/* action=data / end_data with a chunk. Data replies are never quieted. */
static void kt_data(Terminal *term, const char *id, const char *fid,
                    const unsigned char *data, size_t n, int last)
{
    strbuf *sb = strbuf_new();
    put_fmt(sb, "\033]5113;ac=%s;id=%s;fid=%s", last ? "end_data" : "data", id, fid);
    if (n)
        kt_put_b64(sb, "d", data, n);
    kt_send_seq(term, sb);
}

/* One entry of a receive session's listing. */
static void kt_entry_reply(Terminal *term, struct kt_recv *r, const char *spec_fid,
                           const struct kt_entry *e, int64_t mtime_ns, int readonly,
                           const char *parent_rid)
{
    strbuf *sb = strbuf_new();
    put_fmt(sb, "\033]5113;ac=file;id=%s;fid=%s", r->id, spec_fid);
    kt_put_b64(sb, "st", e->rid, strlen(e->rid));
    kt_put_b64(sb, "n", e->posix, strlen(e->posix));
    put_fmt(sb, ";sz=%llu;mod=%lld;prm=%d", (unsigned long long)e->size,
            (long long)mtime_ns,
            e->is_dir ? 0755 : (readonly ? 0444 : 0644));
    if (e->is_dir)
        put_fmt(sb, ";ft=directory");
    if (parent_rid && *parent_rid)
        put_fmt(sb, ";pr=%s", parent_rid);
    kt_send_seq(term, sb);
}

/* ------------------------------------------------------------------------
 * The permission dialog
 * ------------------------------------------------------------------------ */

/* The themed confirm box has Yes/No buttons; this thread-local CBT hook
 * relabels them Allow/Deny the moment our box is activated, matched by its
 * caption so no other dialog is touched. */
static HHOOK kt_cbt = NULL;

static LRESULT CALLBACK kt_cbt_proc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_ACTIVATE) {
        char cap[64];
        HWND h = (HWND)wParam;
        cap[0] = '\0';
        GetWindowTextA(h, cap, sizeof(cap));
        if (!strcmp(cap, KT_XFER5113_CAP)) {
            SetDlgItemTextA(h, IDYES, KT_XFER5113_BTN_ALLOW);
            SetDlgItemTextA(h, IDNO, KT_XFER5113_BTN_DENY);
        }
    }
    return CallNextHookEx(kt_cbt, code, wParam, lParam);
}

static int kt_ask(const char *text, const char *warn)
{
    int yes;
    if (MainHwnd) {
        ShowWindow(MainHwnd, SW_SHOWNA);
        SetForegroundWindow(MainHwnd);
    }
    kt_cbt = SetWindowsHookExA(WH_CBT, kt_cbt_proc, NULL, GetCurrentThreadId());
    yes = kitty_confirm_box(MainHwnd, KT_XFER5113_CAP, text, warn);
    if (kt_cbt) {
        UnhookWindowsHookEx(kt_cbt);
        kt_cbt = NULL;
    }
    return yes;
}

/* ------------------------------------------------------------------------
 * Both request dialogs are MODELESS.
 *
 * A question about a transfer must not stop the terminal: the session keeps
 * drawing, scrolling and taking input while it stands, and the far end's own
 * cancel still arrives. They are created owned by the terminal window and
 * registered with ShinyAddAuxDialog(), which is what keeps Tab and Esc
 * working in whichever message pump happens to be running (window.c's, or
 * ShinyDialogBox's while the configuration box is open).
 *
 * The decision therefore continues in a COMPLETION rather than at the point
 * of asking: kt_send_decide() / kt_recv_decide(), called once the window has
 * gone, whether it went by Allow, by Deny, by the close box or because the
 * terminal died under it.
 *
 * Bringing the window to the front is deliberate, and the theme needs it as
 * well: the engine dresses a dialog when it is ACTIVATED (the CBT hook in
 * kitty_theme.c), so a window shown without activation would come up light
 * inside a dark application.
 * ------------------------------------------------------------------------ */

static HWND kt_dialog_open(int template_id, DLGPROC proc, void *ctx)
{
    HWND h = CreateDialogParamA(GetModuleHandle(NULL),
                                MAKEINTRESOURCEA(template_id), MainHwnd,
                                proc, (LPARAM)ctx);
    if (!h)
        return NULL;                    /* the template did not load */
    ShinyAddAuxDialog(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    return h;
}

/* ------------------------------------------------------------------------
 * The download-request dialog (IDD_XFERDL): the request text with the
 * folder the files land in, the warning line, Allow / Change folder... /
 * Deny. "Change folder..." opens the folder picker on the folder shown; a
 * pick closes the dialog as Allow with that folder, a cancelled picker
 * returns to the dialog unchanged. The theme engine dresses it like every
 * other dialog of this module and paints the warning line from the mark set
 * on it here; the buttons are sized from the captions they carry.
 * ------------------------------------------------------------------------ */

struct kt_dl_dlg {
    struct kt_state *st;                /* the session the answer belongs to */
    char folder[4096];                  /* the folder shown */
    char picked[4096];                  /* set by Change folder... */
    int picked_set;
};

/* The completion: everything kt_send_begin used to do after the dialog. */
static void kt_send_decide(struct kt_state *st, int allowed,
                           const char *folder, int picked);

/*
 * The warning line and the buttons: moved down by `dy` (how far the request
 * text had to grow), the buttons laid out right to left at the width each
 * caption actually needs, and the window widened if that row no longer fits
 * across it.
 *
 * One DeferWindowPos transaction for the whole row, then one redraw of the
 * dialog and every child - the same rule as the shared anchoring
 * (kitty_anchor.c): a control moved on its own with MoveWindow leaves the
 * pixels it vacated behind, and a control RESIZED on its own keeps the bits
 * it had and repaints only the strip that appeared.
 */
static void kt_dl_size_buttons(HWND h, int dy)
{
    static const int ids[] = { IDNO, IDC_XFERDL_CHANGE, IDYES };  /* right to left */
    int w[lenof(ids)];
    RECT rc, r, a, b;
    HWND ha = GetDlgItem(h, IDC_XFERDL_CHANGE), hb = GetDlgItem(h, IDNO);
    HDWP dwp;
    int i, margin, gap, need, x, client_w;

    if (!GetClientRect(h, &rc) || !ha || !hb ||
        !GetWindowRect(ha, &a) || !GetWindowRect(hb, &b))
        return;
    /* The template's own margin and inter-button gap, in this monitor's
     * pixels: taken from where it put the last two buttons. */
    MapWindowPoints(NULL, h, (POINT *)&a, 2);
    MapWindowPoints(NULL, h, (POINT *)&b, 2);
    margin = rc.right - b.right;
    gap = b.left - a.right;
    if (gap < 0)
        gap = 0;

    need = 2 * margin + gap * ((int)lenof(ids) - 1);
    for (i = 0; i < (int)lenof(ids); i++) {
        HWND c = GetDlgItem(h, ids[i]);
        if (!c || !GetWindowRect(c, &r))
            return;
        w[i] = kitty_theme_button_width(c, r.right - r.left);
        need += w[i];
    }
    /* The row of captions is wider than the window: widen the window rather
     * than let a caption spill out of its button. */
    client_w = rc.right;
    if (need > client_w) {
        RECT wr;
        GetWindowRect(h, &wr);
        SetWindowPos(h, NULL, 0, 0, (wr.right - wr.left) + (need - client_w),
                     wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
        GetClientRect(h, &rc);
        client_w = rc.right;
    }
    x = client_w - margin;
    dwp = BeginDeferWindowPos((int)lenof(ids) + 1);
    for (i = 0; dwp && i < (int)lenof(ids); i++) {
        HWND c = GetDlgItem(h, ids[i]);
        if (!c || !GetWindowRect(c, &r))
            continue;
        MapWindowPoints(NULL, h, (POINT *)&r, 2);
        x -= w[i];
        dwp = DeferWindowPos(dwp, c, NULL, x, r.top + dy, w[i],
                             r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        x -= gap;
    }
    if (dwp) {
        HWND warn = GetDlgItem(h, IDC_XFERDL_WARN);
        if (warn && dy != 0 && GetWindowRect(warn, &r)) {
            MapWindowPoints(NULL, h, (POINT *)&r, 2);
            dwp = DeferWindowPos(dwp, warn, NULL, r.left, r.top + dy,
                                 r.right - r.left, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (dwp)
        EndDeferWindowPos(dwp);
    RedrawWindow(h, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static INT_PTR CALLBACK kt_dl_dlgproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* The verdict is delivered AFTER the window has gone, never from inside
 * WM_DESTROY: the completion opens the folder picker in some cases, and a
 * dying dialog is no owner for one. */
static void kt_dl_finish(HWND h, int verdict)
{
    struct kt_dl_dlg *d = (struct kt_dl_dlg *)GetWindowLongPtr(h, GWLP_USERDATA);
    struct kt_state *st;
    char folder[4096];
    int picked;

    if (!d)
        return;
    st = d->st;
    picked = verdict && d->picked_set;
    strcpy(folder, picked ? d->picked : d->folder);
    DestroyWindow(h);                   /* WM_DESTROY unregisters and frees d */
    kt_send_decide(st, verdict, folder, picked);
}

static INT_PTR CALLBACK kt_dl_dlgproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct kt_dl_dlg *d = (struct kt_dl_dlg *)GetWindowLongPtr(h, GWLP_USERDATA);
    switch (msg) {
      case WM_INITDIALOG: {
        char *text;
        int dh;

        d = (struct kt_dl_dlg *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        SetWindowTextA(h, KT_XFER5113_CAP);
        text = dupprintf(KT_XFER5113_ASK_SEND, d->folder);
        SetDlgItemTextA(h, IDC_XFERDL_TEXT, text);
        SetDlgItemTextA(h, IDC_XFERDL_WARN, KT_XFER5113_ASK_SEND_WARN);
        SetDlgItemTextA(h, IDYES, KT_XFER5113_BTN_ALLOW);
        SetDlgItemTextA(h, IDC_XFERDL_CHANGE, KT_XFER5113_BTN_CHANGE_FOLDER);
        SetDlgItemTextA(h, IDNO, KT_XFER5113_BTN_DENY);
        /* The warning line is painted by the theme engine, in the ink for the
         * theme in force - not by a colour written here (kitty_theme.h). */
        kitty_theme_mark_ink(GetDlgItem(h, IDC_XFERDL_WARN), KITTY_INK_BAD);
        /* And the caption and taskbar icon of the terminal that raised it -
         * this session's own icon when it carries one. */
        kitty_dialog_icon(h, MainHwnd);
        /* A long folder path wraps: grow the text to fit, as the confirm box
         * does, and grow the window by as much. */
        dh = kitty_fit_text(h, IDC_XFERDL_TEXT, text, 0);
        sfree(text);
        if (dh != 0) {
            RECT wr;
            GetWindowRect(h, &wr);
            SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) + dh,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        /* Everything below the text, in one go: moved down by that much, the
         * buttons sized from the captions set above and never from the
         * template's widths. */
        kt_dl_size_buttons(h, dh);
        kitty_centre_on_owner(h);
        SetFocus(GetDlgItem(h, IDYES));     /* Return means Allow */
        return FALSE;
      }
      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDYES:
            kt_dl_finish(h, 1);
            return TRUE;
          case IDC_XFERDL_CHANGE:
            /* The same picker "Always open Save Dialog" uses, owned by this
             * dialog and opened on the folder shown. A pick is an Allow into
             * that folder; Cancel leaves the dialog as it was. */
            if (!d)
                return TRUE;
            if (OpenDirNameFrom(h, d->picked, d->folder, KT_XFER5113_PICK_TITLE) &&
                d->picked[0]) {
                d->picked_set = 1;
                kt_dl_finish(h, 1);
            } else if (d->st && d->st->send &&
                       (d->st->send->cancel_pending || d->st->send->abort_pending)) {
                /* The far end gave up while the picker was open: the WM_CLOSE
                 * posted for it was refused below, so take the window down
                 * now that the picker has gone. */
                kt_dl_finish(h, 0);
            }
            return TRUE;
          case IDNO:
          case IDCANCEL:
            kt_dl_finish(h, 0);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        /* Not while a picker of ours is up: it owns this window, and
         * destroying the owner of a live common dialog frees state it is
         * still standing on. The picker's own return path closes instead. */
        if (!IsWindowEnabled(h))
            return TRUE;
        kt_dl_finish(h, 0);             /* closing means Deny */
        return TRUE;
      case WM_DESTROY:
        /* Reached from kt_dl_finish and from the terminal going away. The
         * completion is NOT called here - see kt_dl_finish. */
        ShinyRemoveAuxDialog(h);
        if (d) {
            if (d->st && d->st->send && d->st->send->dlg == h)
                d->st->send->dlg = NULL;
            sfree(d);
        }
        SetWindowLongPtr(h, GWLP_USERDATA, 0);
        return FALSE;                   /* the manager tidies up as well */
    }
    return FALSE;
}

/* Put the dialog up. True when it stands and the answer will arrive in
 * kt_send_decide(); false when the template did not load at all, which
 * leaves the caller to ask the same question in the confirm box. */
static int kt_dl_dialog_open(struct kt_state *st, const char *folder)
{
    struct kt_dl_dlg *d = snew(struct kt_dl_dlg);
    HWND h;

    memset(d, 0, sizeof(*d));
    d->st = st;
    strncpy(d->folder, folder, sizeof(d->folder) - 1);
    h = kt_dialog_open(IDD_XFERDL, kt_dl_dlgproc, d);
    if (!h) {
        sfree(d);
        return 0;
    }
    st->send->dlg = h;
    return 1;
}

/* ------------------------------------------------------------------------
 * Send sessions: files arriving from the far end
 * ------------------------------------------------------------------------ */

static void kt_file_free(struct kt_file *f)
{
    if (f->h != INVALID_HANDLE_VALUE)
        CloseHandle(f->h);
    if (!f->done && f->part_path)
        DeleteFileW(f->part_path);      /* never leave a half file behind */
    if (f->dec)
        ssh_decompressor_free(f->dec);
    sfree(f->final_path);
    sfree(f->part_path);
    sfree(f->posix);
    sfree(f);
}

static void kt_send_free(struct kt_state *st)
{
    struct kt_send *s = st->send;
    struct kt_file *f, *fn;
    struct kt_dir *d, *dn;
    if (!s)
        return;
    st->send = NULL;
    for (f = s->files; f; f = fn) { fn = f->next; kt_file_free(f); }
    for (d = s->dirs; d; d = dn) { dn = d->next; sfree(d->name); sfree(d->local); sfree(d); }
    sfree(s->dest);
    sfree(s);
}

static struct kt_file *kt_find_file(struct kt_send *s, const char *fid)
{
    struct kt_file *f;
    for (f = s->files; f; f = f->next)
        if (!strcmp(f->fid, fid))
            return f;
    return NULL;
}

/*
 * Where a name from the far end lands. Directory components are never taken
 * from the wire as a path of their own: a file goes into the destination
 * under its last component, unless this session already declared a directory
 * that contains it, in which case it keeps its place under that directory's
 * local folder (every component checked, intermediate folders created). A
 * name that fails the rules yields NULL with *why = 0; a folder that could
 * not be created yields NULL with *why = 1.
 */
static wchar_t *kt_place(struct kt_send *s, const char *name, size_t n, int *why)
{
    struct kt_dir *d, *best = NULL;
    const char *rel = NULL;
    size_t rn = 0, i;
    const wchar_t *base;
    wchar_t *path;

    *why = 0;
    for (d = s->dirs; d; d = d->next) {
        const char *r; size_t l;
        if (kt5113_under(d->name, d->name_len, name, n, &r, &l) &&
            (!best || d->name_len > best->name_len)) {
            best = d; rel = r; rn = l;
        }
    }
    if (best) {
        if (!kt5113_rel_ok(rel, rn))
            return NULL;
        base = best->local;
    } else {
        if (!kt5113_basename(name, n, &rel, &rn))
            return NULL;
        base = s->dest;
    }
    path = kt_wdup(base);
    i = 0;
    while (i < rn) {
        size_t st = i;
        wchar_t *comp, *next;
        while (i < rn && rel[i] != '/')
            i++;
        comp = kt_mb_to_wide(CP_UTF8, rel + st, (int)(i - st));
        if (!comp) {
            sfree(path);
            return NULL;
        }
        next = kt_wjoin(path, comp);
        sfree(comp);
        sfree(path);
        path = next;
        if (i < rn) {
            i++;                        /* an intermediate folder */
            if (!kt_is_dir(path)) {
                if (kt_exists(path) || !CreateDirectoryW(path, NULL)) {
                    sfree(path);
                    *why = 1;
                    return NULL;
                }
            }
        }
    }
    return path;
}

static void kt_file_fail(struct kt_state *st, struct kt_file *f,
                         const char *code, const char *msg)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;
    char *a;
    if (f->h != INVALID_HANDLE_VALUE) {
        CloseHandle(f->h);
        f->h = INVALID_HANDLE_VALUE;
    }
    if (f->part_path)
        DeleteFileW(f->part_path);
    f->failed = 1;
    kt_err(term, s, f->fid, code, msg);
    a = f->final_path ? kt_ansi(f->final_path) : dupstr(f->fid);
    kt_log(term, dupprintf(KT_XFER5113_LOG_FILE_FAILED, s->id, a, msg));
    sfree(a);
}

/* Write inflated (or plain) bytes to the .part file. 0 on failure. */
static int kt_file_write(struct kt_file *f, const unsigned char *p, size_t n,
                         const char **code, const char **msg)
{
    while (n > 0) {
        DWORD chunk = n > (1u << 20) ? (1u << 20) : (DWORD)n, got = 0;
        /* "Max transfer size (MB)": 0 = no limit, really none. */
        if (f->ceiling && f->written + chunk > f->ceiling) {
            *code = "EFBIG"; *msg = KT_XFER5113_ST_TOO_LARGE;
            return 0;
        }
        if (!WriteFile(f->h, p, chunk, &got, NULL) || got != chunk) {
            *code = "EIO"; *msg = KT_XFER5113_ST_WRITE_FAILED;
            return 0;
        }
        f->adler = kt5113_adler32(f->adler, p, chunk);
        f->written += chunk;
        p += chunk;
        n -= chunk;
    }
    return 1;
}

/*
 * Feed one chunk. For a zlib stream the last four bytes are the Adler-32
 * trailer; PuTTY's inflater treats the byte after the final block as the next
 * block header, so those four are held back - the tail buffer keeps the most
 * recent four bytes across chunks - and checked here against our own sum at
 * end_data. That check is also what turns a truncated or corrupt stream into
 * an error rather than a short file.
 */
static int kt_file_feed(struct kt_file *f, const unsigned char *data, size_t n,
                        int last, const char **code, const char **msg)
{
    unsigned char *buf, *out = NULL;
    size_t total, feed;
    int outlen = 0, ok = 1;

    if (!f->dec)
        return kt_file_write(f, data, n, code, msg);

    total = (size_t)f->tail_len + n;
    buf = snewn(total + 1, unsigned char);
    memcpy(buf, f->tail, (size_t)f->tail_len);
    memcpy(buf + f->tail_len, data, n);
    feed = total > 4 ? total - 4 : 0;
    if (feed) {
        if (!ssh_decompressor_decompress(f->dec, buf, (int)feed, &out, &outlen)) {
            *code = "EINVAL"; *msg = KT_XFER5113_ST_ZLIB_CORRUPT;
            ok = 0;
        } else {
            if (outlen > 0)
                ok = kt_file_write(f, out, (size_t)outlen, code, msg);
            sfree(out);
        }
    }
    f->tail_len = (int)(total - feed);
    memcpy(f->tail, buf + feed, (size_t)f->tail_len);
    sfree(buf);
    if (ok && last) {
        uint32_t want;
        if (f->tail_len != 4) {
            *code = "EINVAL"; *msg = KT_XFER5113_ST_ZLIB_CORRUPT;
            return 0;
        }
        want = ((uint32_t)f->tail[0] << 24) | ((uint32_t)f->tail[1] << 16) |
               ((uint32_t)f->tail[2] << 8) | f->tail[3];
        if (want != f->adler) {
            *code = "EINVAL"; *msg = KT_XFER5113_ST_ZLIB_CORRUPT;
            return 0;
        }
    }
    return ok;
}

/* end_data arrived and was written: stamp, close, rename into place. */
static void kt_file_finish(struct kt_state *st, struct kt_file *f)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;
    char *a;

    if (f->mtime >= 0) {
        FILETIME ft;
        kt_ns_to_filetime(f->mtime, &ft);
        SetFileTime(f->h, NULL, NULL, &ft);
    }
    CloseHandle(f->h);
    f->h = INVALID_HANDLE_VALUE;
    if (kt_exists(f->final_path)) {
        /* something took the name while the data was arriving */
        wchar_t *alt = kt_unique(f->final_path);
        if (!alt) {
            kt_file_fail(st, f, "EIO", KT_XFER5113_ST_CREATE_FAILED);
            return;
        }
        sfree(f->final_path);
        f->final_path = alt;
        sfree(f->posix);
        f->posix = kt_posix(alt);
    }
    if (!MoveFileExW(f->part_path, f->final_path, 0)) {
        kt_file_fail(st, f, "EIO", KT_XFER5113_ST_WRITE_FAILED);
        return;
    }
    if (f->readonly)
        SetFileAttributesW(f->final_path,
                           kt_attrs(f->final_path) | FILE_ATTRIBUTE_READONLY);
    f->done = 1;
    s->ndone++;
    kt_ack(term, s, f->fid, "OK", f->posix, (int64_t)f->written);
    a = kt_ansi(f->final_path);
    kt_log(term, dupprintf(KT_XFER5113_LOG_FILE_DONE, s->id, a,
                           (unsigned long long)f->written));
    sfree(a);
}

/* action=file in a send session: the metadata of one entry. */
static void kt_send_file(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;
    struct kt_file *f;
    wchar_t *path;
    int why = 0;

    if (!c->fid[0])
        return;                         /* nothing to answer against */
    if (kt_find_file(s, c->fid)) {
        kt_err(term, s, c->fid, "EINVAL", KT_XFER5113_ST_DUP_FID);
        return;
    }
    f = snew(struct kt_file);
    memset(f, 0, sizeof(*f));
    strcpy(f->fid, c->fid);
    f->ftype = c->ftype;
    f->refused = 1;
    f->h = INVALID_HANDLE_VALUE;
    f->ceiling = s->ceiling;
    f->mtime = -1;
    f->adler = 1;
    f->next = s->files;
    s->files = f;
    s->nfiles++;

    if (s->nfiles > KT_MAX_FILES) {
        kt_err(term, s, c->fid, "EINVAL", KT_XFER5113_ST_TOO_MANY);
        return;
    }
    if (c->ftype == KT5113_FT_SYMLINK || c->ftype == KT5113_FT_LINK) {
        kt_err(term, s, c->fid, "ENOTSUP", KT_XFER5113_ST_LINKS);
        kt_log(term, dupprintf(KT_XFER5113_LOG_FILE_REFUSED, s->id,
                               c->name_bad ? "?" : c->name, KT_XFER5113_ST_LINKS));
        return;
    }
    if (c->ftype == KT5113_FT_UNKNOWN) {
        kt_err(term, s, c->fid, "EINVAL", KT_XFER5113_ST_BAD_FTYPE);
        return;
    }
    if (c->zip == KT5113_ZIP_UNKNOWN) {
        kt_err(term, s, c->fid, "EINVAL", KT_XFER5113_ST_BAD_ZIP);
        return;
    }
    if (c->name_bad || c->name_len == 0 ||
        (path = kt_place(s, c->name, c->name_len, &why)) == NULL) {
        const char *m = why ? KT_XFER5113_ST_MKDIR_FAILED : KT_XFER5113_ST_BAD_NAME;
        kt_err(term, s, c->fid, why ? "EIO" : "EINVAL", m);
        kt_log(term, dupprintf(KT_XFER5113_LOG_FILE_REFUSED, s->id,
                               c->name_bad ? "?" : c->name, m));
        return;
    }

    if (c->ftype == KT5113_FT_DIRECTORY) {
        struct kt_dir *d;
        size_t nl = c->name_len;
        char *a;
        if (!kt_is_dir(path)) {
            if (kt_exists(path) || !CreateDirectoryW(path, NULL)) {
                sfree(path);
                kt_err(term, s, c->fid, "EIO", KT_XFER5113_ST_MKDIR_FAILED);
                return;
            }
        }
        while (nl > 1 && c->name[nl - 1] == '/')
            nl--;
        d = snew(struct kt_dir);
        d->name = snewn(nl + 1, char);
        memcpy(d->name, c->name, nl);
        d->name[nl] = '\0';
        d->name_len = nl;
        d->local = path;
        d->next = s->dirs;
        s->dirs = d;
        f->refused = 0;
        f->done = 1;
        f->posix = kt_posix(path);
        kt_ack(term, s, c->fid, "OK", f->posix, -1);
        a = kt_ansi(path);
        kt_log(term, dupprintf(KT_XFER5113_LOG_DIR_MADE, s->id, a));
        sfree(a);
        return;
    }

    f->final_path = kt_unique(path);
    sfree(path);
    if (!f->final_path) {
        kt_err(term, s, c->fid, "EIO", KT_XFER5113_ST_CREATE_FAILED);
        return;
    }
    f->part_path = kt_part_name(f->final_path);
    f->h = CreateFileW(f->part_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f->h == INVALID_HANDLE_VALUE) {
        kt_err(term, s, c->fid, "EIO", KT_XFER5113_ST_CREATE_FAILED);
        sfree(f->part_path);
        f->part_path = NULL;            /* nothing to delete */
        return;
    }
    f->refused = 0;
    f->posix = kt_posix(f->final_path);
    f->mtime = c->has_mtime && c->mtime >= 0 ? c->mtime : -1;
    f->readonly = c->has_perms && !(c->perms & 0200);
    if (c->zip == KT5113_ZIP_ZLIB)
        f->dec = ssh_decompressor_new(&ssh_zlib);
    /* STARTED without tt=: a client that asked for rsync falls back to
     * plain data on its own. */
    kt_ack(term, s, c->fid, "STARTED", f->posix, -1);
    {
        char *a = kt_ansi(f->final_path);
        kt_log(term, dupprintf(KT_XFER5113_LOG_FILE_START, s->id, a));
        sfree(a);
    }
}

/* action=data / end_data in a send session. */
static void kt_send_data(struct kt_state *st, const kt5113_cmd *c, int last)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;
    struct kt_file *f = c->fid[0] ? kt_find_file(s, c->fid) : NULL;
    const char *code = "EIO", *msg = KT_XFER5113_ST_WRITE_FAILED;
    uint64_t before;

    if (f && !f->refused && f->ftype == KT5113_FT_DIRECTORY) {
        kt_err(term, s, c->fid, "EISDIR", KT_XFER5113_ST_ISDIR);
        return;
    }
    if (!f || f->refused || f->done || f->failed)
        return;                         /* not STARTED: discarded, as the spec says */
    if (c->data_bad) {
        kt_file_fail(st, f, "EINVAL", KT_XFER5113_ST_BAD_DATA);
        return;
    }
    before = f->written;
    if (!kt_file_feed(f, c->data, c->data_len, last, &code, &msg)) {
        kt_file_fail(st, f, code, msg);
        return;
    }
    if (last)
        kt_file_finish(st, f);
    else if (f->written > before)
        kt_ack(term, s, c->fid, "PROGRESS", NULL, (int64_t)f->written);
}

/* action=send: a new session. The dialog is put up here and the OK that lets
 * the client continue is sent from kt_send_decide(), once it has been
 * answered - the terminal keeps running in between. */
static void kt_send_begin(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_send *s;
    char folder[4096];
    int policy;

    if (st->send) {
        if (!strcmp(st->send->id, c->id)) {
            /* the reference drops a session whose id starts again */
            kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, c->id));
            kt_send_free(st);
        } else {
            kt_status(term, c->quiet, 1, c->id, NULL, "EPERM", KT_XFER5113_ST_BUSY, NULL, -1);
            kt_log(term, dupprintf(KT_XFER5113_LOG_BUSY, c->id));
            return;
        }
    }
    s = snew(struct kt_send);
    memset(s, 0, sizeof(*s));
    strcpy(s->id, c->id);
    s->quiet = c->quiet;
    s->last = time(NULL);
    s->ceiling = kt_max_bytes(term->conf);
    st->send = s;                       /* registered before the dialog: commands
                                         * arriving meanwhile must find it */

    kitty_xfer_download_dir(term->conf, folder, sizeof(folder));
    policy = conf_get_int(term->conf, CONF_xfer_permission);
    if (policy == 2 || (policy == 1 && st->latched)) {
        kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_AUTO, s->id));
        kt_send_decide(st, 1, folder, 0);
        return;
    }
    kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_ASK, s->id, folder));
    s->in_dialog = 1;                   /* a command arriving now is held over */
    if (kt_dl_dialog_open(st, folder))
        return;                         /* answered in kt_send_decide */
    /* The template did not load at all: the same question in the suite's
     * shared confirm box (Allow / Deny, no folder change), which is modal by
     * design. Never a silent allow. */
    {
        char *text = dupprintf(KT_XFER5113_ASK_SEND, folder);
        int yes = kt_ask(text, KT_XFER5113_ASK_SEND_WARN);
        sfree(text);
        kt_send_decide(st, yes, folder, 0);
    }
}

/*
 * The decision, once the request dialog has gone - or straight away when the
 * permission setting answered without asking. `folder` is where the files
 * land; `picked` says it came from "Change folder...", which is what makes
 * the "Always open Save Dialog" picker stand down.
 */
static void kt_send_decide(struct kt_state *st, int allowed,
                           const char *folder_in, int picked)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;
    char folder[4096], *a;

    if (!s)
        return;                         /* the session went while the dialog stood */
    s->in_dialog = 0;
    s->dlg = NULL;
    if (st->dead) {                     /* the window went away under the dialog */
        kt_send_free(st);
        kt_state_maybe_drop(st);
        return;
    }
    if (allowed && conf_get_int(term->conf, CONF_xfer_permission) == 1)
        st->latched = 1;
    strncpy(folder, folder_in, sizeof(folder) - 1);
    folder[sizeof(folder) - 1] = '\0';
    if (s->cancel_pending) {
        kt_ack(term, s, NULL, "CANCELED", NULL, -1);
        kt_log(term, dupprintf(KT_XFER5113_LOG_CANCELLED, s->id));
        kt_send_free(st);
        return;
    }
    if (s->abort_pending) {
        kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, s->id));
        kt_send_free(st);
        return;
    }
    if (!allowed) {
        kt_err(term, s, NULL, "EPERM", KT_XFER5113_ST_REFUSED);
        kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_DENIED, s->id));
        kt_send_free(st);
        return;
    }
    /* "Always open Save Dialog": the picker before the first file, unless
     * the dialog's Change folder... already produced the folder. */
    if (conf_get_bool(term->conf, CONF_xfer_ask_destination) && !picked) {
        char chosen[4096];
        int ok;
        s->in_dialog = 1;
        ok = OpenDirNameFrom(MainHwnd, chosen, folder, KT_XFER5113_PICK_TITLE);
        s->in_dialog = 0;
        if (st->dead) {
            kt_send_free(st);
            kt_state_maybe_drop(st);
            return;
        }
        if (s->cancel_pending || s->abort_pending) {
            if (s->cancel_pending)
                kt_ack(term, s, NULL, "CANCELED", NULL, -1);
            kt_log(term, dupprintf(KT_XFER5113_LOG_CANCELLED, s->id));
            kt_send_free(st);
            return;
        }
        if (!ok) {
            kt_err(term, s, NULL, "EPERM", KT_XFER5113_ST_REFUSED);
            kt_log(term, dupprintf(KT_XFER5113_LOG_NO_DEST, s->id));
            kt_send_free(st);
            return;
        }
        strncpy(folder, chosen, sizeof(folder) - 1);
        folder[sizeof(folder) - 1] = '\0';
    }
    s->dest = kt_mb_to_wide(CP_ACP, folder, -1);
    if (!s->dest || !kt_is_dir(s->dest)) {
        kt_err(term, s, NULL, "ENOENT", KT_XFER5113_ST_NOT_FOUND);
        kt_log(term, dupprintf(KT_XFER5113_LOG_NO_DEST, s->id));
        kt_send_free(st);
        return;
    }
    s->accepted = 1;
    kt_ack(term, s, NULL, "OK", NULL, -1);
    a = kt_ansi(s->dest);
    kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_OK, s->id, a));
    sfree(a);
}

static void kt_send_cmd(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_send *s = st->send;

    if (s->in_dialog) {
        /* The request dialog still stands. The spec: a command before OK
         * drops the session; a cancel is answered afterwards. */
        if (c->action == KT5113_AC_CANCEL)
            s->cancel_pending = 1;
        else
            s->abort_pending = 1;
        /* Take the question down - it is about a transfer the far end has
         * already given up on. Posted, not destroyed: we are inside the
         * escape-sequence parser, under the terminal's own window. */
        if (s->dlg)
            PostMessage(s->dlg, WM_CLOSE, 0, 0);
        return;
    }
    s->last = time(NULL);
    switch (c->action) {
      case KT5113_AC_CANCEL:
        kt_ack(term, s, NULL, "CANCELED", NULL, -1);
        kt_log(term, dupprintf(KT_XFER5113_LOG_CANCELLED, s->id));
        kt_send_free(st);
        break;
      case KT5113_AC_FINISH:
        kt_log(term, dupprintf(KT_XFER5113_LOG_FINISHED, s->id, s->ndone));
        if (s->ndone > 0) {             /* [KiTTY] transfernotification */
            /* The balloon's click opens the one file that landed, or the
             * folder of the transfer (the root of everything that landed). */
            struct kt_file *f;
            char *path = NULL;
            if (s->ndone == 1)
                for (f = s->files; f; f = f->next)
                    if (f->done && f->final_path) {
                        path = kt_ansi(f->final_path);
                        break;
                    }
            if (!path && s->dest)
                path = kt_ansi(s->dest);
            kitty_xfer_notify(KT_XFER_WHAT_KITTEN_RECV, 1, s->ndone, path);
            sfree(path);
        }
        kt_send_free(st);               /* incomplete .part files go with it */
        break;
      case KT5113_AC_FILE:
        kt_send_file(st, c);
        break;
      case KT5113_AC_DATA:
        kt_send_data(st, c, 0);
        break;
      case KT5113_AC_END_DATA:
        kt_send_data(st, c, 1);
        break;
      case KT5113_AC_SEND:
        kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, s->id));
        kt_send_free(st);
        break;
      default:
        break;                          /* status/receive/unknown: ignored */
    }
}

/* ------------------------------------------------------------------------
 * Receive sessions: files leaving for the far end
 * ------------------------------------------------------------------------ */

static void kt_req_free(struct kt_req *q)
{
    if (q->h != INVALID_HANDLE_VALUE)
        CloseHandle(q->h);
    sfree(q);
}

static void kt_recv_free(struct kt_state *st)
{
    struct kt_recv *r = st->recv;
    struct kt_entry *e, *en;
    struct kt_req *q, *qn;
    int i;
    if (!r)
        return;
    st->recv = NULL;
    expire_timer_context(st);
    for (q = r->queue; q; q = qn) { qn = q->next; kt_req_free(q); }
    for (e = r->entries; e; e = en) { en = e->next; sfree(e->posix); sfree(e->local); sfree(e); }
    for (i = 0; i < r->got; i++)
        sfree(r->specs[i].name);
    sfree(r->specs);
    sfree(r);
}

/* A path the far end asks for, as a local path, or NULL when it is not one we
 * would ever open: everything is taken relative to the UPLOAD folder except
 * the spec's own /C:/... form, which names a drive path outright and is
 * honoured only with "Allow full path Upload-Requests" on - refused here,
 * before any dialog, and *full_refused says so. The dialog shows exactly
 * what was resolved. */
static wchar_t *kt_resolve_spec(Terminal *term, const char *name, int *full_refused)
{
    const char *rel;
    size_t rn;
    char drive;
    int kind = kt5113_spec_classify(name, strlen(name), &rel, &rn, &drive);
    wchar_t *base, *path, *w;

    if (full_refused)
        *full_refused = 0;
    if (kind == KT5113_SPEC_BAD)
        return NULL;
    if (kind == KT5113_SPEC_DRIVE) {
        wchar_t root[4] = { (wchar_t)drive, L':', L'\\', 0 };
        if (!kt_full_path_allowed(term->conf)) {
            if (full_refused)
                *full_refused = 1;
            return NULL;
        }
        base = kt_wdup(root);
    } else {
        char folder[4096];
        kitty_xfer_upload_dir(term->conf, folder, sizeof(folder));
        base = kt_mb_to_wide(CP_ACP, folder, -1);
        if (!base)
            return NULL;
    }
    if (rn == 0)
        return base;
    w = kt_mb_to_wide(CP_UTF8, rel, (int)rn);
    if (!w) {
        sfree(base);
        return NULL;
    }
    {
        wchar_t *p;
        for (p = w; *p; p++)
            if (*p == L'/')
                *p = L'\\';
    }
    path = kt_wjoin(base, w);
    sfree(base);
    sfree(w);
    return path;
}

static struct kt_entry *kt_entry_add(struct kt_recv *r, const wchar_t *local,
                                     int is_dir, uint64_t size,
                                     const char *spec_fid, struct kt_entry *parent,
                                     int64_t mtime_ns, int readonly)
{
    struct kt_entry *e = snew(struct kt_entry), **pp;
    memset(e, 0, sizeof(*e));
    snprintf(e->rid, sizeof(e->rid), "%llu", (unsigned long long)++r->rid_counter);
    strncpy(e->spec_fid, spec_fid, sizeof(e->spec_fid) - 1);
    e->local = kt_wdup(local);
    e->posix = kt_posix(local);
    e->is_dir = is_dir;
    e->size = size;
    e->parent = parent;
    e->mtime_ns = mtime_ns;
    e->readonly = readonly;
    for (pp = &r->entries; *pp; pp = &(*pp)->next)
        ;
    *pp = e;
    r->nentries++;
    if (!is_dir) {
        r->nfiles++;
        r->total_bytes += size;
    }
    return e;
}

/*
 * A requested name with nothing behind it on this PC. It becomes a row of the
 * dialog - unchecked and marked - rather than nothing at all, so a request
 * that names a file we do not have is VISIBLE and can be answered by putting
 * a local file in its place ("Locate..."). It is not counted as a file and is
 * never sent to the far end while it stays like this: the spec keeps its
 * ENOENT and that is what the host is told.
 */
static struct kt_entry *kt_entry_add_missing(struct kt_recv *r,
                                             const struct kt_spec *sp)
{
    struct kt_entry *e = snew(struct kt_entry), **pp;
    memset(e, 0, sizeof(*e));
    snprintf(e->rid, sizeof(e->rid), "%llu", (unsigned long long)++r->rid_counter);
    strncpy(e->spec_fid, sp->fid, sizeof(e->spec_fid) - 1);
    e->posix = dupstr(sp->name);        /* the name that was ASKED for */
    e->mtime_ns = -1;
    e->missing = 1;
    e->denied = 1;                      /* nothing to send until it is located */
    for (pp = &r->entries; *pp; pp = &(*pp)->next)
        ;
    *pp = e;
    r->nentries++;
    return e;
}

static struct kt_spec *kt_spec_by_fid(struct kt_recv *r, const char *fid)
{
    int i;
    for (i = 0; i < r->got; i++)
        if (!strcmp(r->specs[i].fid, fid))
            return &r->specs[i];
    return NULL;
}

/* Walk one folder (recursively), an entry per item, BEFORE the dialog - the
 * dialog lists every file that would leave. Reparse points - symlinks,
 * junctions - are skipped and never followed. Returns 0 when the per-session
 * ceiling was hit. */
static int kt_walk_dir(struct kt_state *st, const char *spec_fid,
                       const wchar_t *dir, struct kt_entry *parent, int depth)
{
    struct kt_recv *r = st->recv;
    wchar_t *pattern = kt_wjoin(dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    sfree(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        wchar_t *child;
        struct kt_entry *e;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (r->nentries >= KT_MAX_FILES) {
            FindClose(h);
            return 0;
        }
        child = kt_wjoin(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e = kt_entry_add(r, child, 1, 0, spec_fid, parent,
                             kt_filetime_to_ns(&fd.ftLastWriteTime), 0);
            if (depth < KT_MAX_DEPTH && !kt_walk_dir(st, spec_fid, child, e, depth + 1)) {
                sfree(child);
                FindClose(h);
                return 0;
            }
        } else {
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            kt_entry_add(r, child, 0, sz, spec_fid, parent,
                         kt_filetime_to_ns(&fd.ftLastWriteTime),
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);
        }
        sfree(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* Resolve every spec and walk the folders, before anything is shown or
 * answered: a spec that cannot be served gets its reason recorded (sent
 * after the OK, per file - the others continue), everything else becomes
 * entries the dialog can list. */
static void kt_recv_prepare(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    int i, stop = 0;

    for (i = 0; i < r->got && !stop; i++) {
        struct kt_spec *sp = &r->specs[i];
        int full = 0;
        wchar_t *local = kt_resolve_spec(term, sp->name, &full);
        DWORD attrs = 0;
        WIN32_FILE_ATTRIBUTE_DATA ad;

        sp->why = NULL;
        sp->code = "EINVAL";
        if (!local) {
            if (full) {
                sp->why = KT_XFER5113_ST_FULL_PATH; sp->code = "EPERM";
            } else {
                sp->why = KT_XFER5113_ST_OUTSIDE;
            }
        } else if (!GetFileAttributesExW(local, GetFileExInfoStandard, &ad)) {
            /* Not here. The dialog still shows the request, as a row that can
             * be answered with a local file of the user's choosing; the
             * refusal below stands unless one is. */
            sp->why = KT_XFER5113_ST_NOT_FOUND; sp->code = "ENOENT";
            kt_entry_add_missing(r, sp);
        } else if ((attrs = ad.dwFileAttributes) & FILE_ATTRIBUTE_REPARSE_POINT) {
            sp->why = KT_XFER5113_ST_LINKS; sp->code = "ENOTSUP";
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            struct kt_entry *e = kt_entry_add(r, local, 1, 0, sp->fid, NULL,
                                              kt_filetime_to_ns(&ad.ftLastWriteTime), 0);
            if (!kt_walk_dir(st, sp->fid, local, e, 1)) {
                sp->why = KT_XFER5113_ST_TOO_MANY;
                stop = 1;
            }
        } else {
            uint64_t sz = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            kt_entry_add(r, local, 0, sz, sp->fid, NULL,
                         kt_filetime_to_ns(&ad.ftLastWriteTime),
                         (attrs & FILE_ATTRIBUTE_READONLY) != 0);
        }
        if (sp->why)
            kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_SPEC, r->id, sp->name, sp->why));
        sfree(local);
    }
}

/* The per-spec refusals recorded by the walk, each against its own fid. */
static void kt_recv_send_spec_errors(struct kt_state *st)
{
    struct kt_recv *r = st->recv;
    int i;
    for (i = 0; i < r->got; i++)
        if (r->specs[i].why)
            kt_err(st->term, r, r->specs[i].fid, r->specs[i].code, r->specs[i].why);
}

/* After permission: the refusals, the metadata of everything walked
 * (denied files included - they are refused when asked for, so the host
 * sees a per-file answer), then OK with the home folder, as the spec's
 * receive flow prescribes. */
static void kt_recv_list(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    struct kt_entry *e;

    kt_recv_send_spec_errors(st);
    for (e = r->entries; e; e = e->next) {
        if (e->missing)
            continue;                   /* a request nothing was put behind */
        kt_entry_reply(term, r, e->spec_fid, e, e->mtime_ns, e->readonly,
                       e->parent ? e->parent->rid : NULL);
    }
    {
        const char *prof = getenv("USERPROFILE");
        wchar_t *w = prof && *prof ? kt_mb_to_wide(CP_ACP, prof, -1) : NULL;
        char *home = w ? kt_posix(w) : dupstr("/");
        kt_ack(term, r, NULL, "OK", home, -1);
        sfree(home);
        sfree(w);
    }
    r->listed = 1;
    kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_OK, r->id, r->nentries));
}

/* ------------------------------------------------------------------------
 * The upload-request dialog (IDD_XFERREQ): one line per file that would
 * leave, a checkbox in front of each, all checked at open; a count line
 * above; "Locate..." / "Allow selected" / "Deny". MODELESS, and resizable in
 * both directions - the list takes whatever the window grows by.
 *
 * A requested name with no file behind it on this PC is a row as well,
 * marked and unchecked, so the request is visible instead of silently
 * producing nothing. "Locate..." puts a local file behind such a row; it is
 * then served under the name the far end ASKED for, so a kitten unpacking
 * into "." still puts it where it meant to.
 *
 * Nothing here paints, sizes or re-places a control by hand. The theme engine
 * dresses the window (kitty_theme.c), the warning line carries an ink mark
 * instead of a colour written here, every button is as wide as the caption it
 * is CARRYING (kitty_theme_button_width), and the resize is the suite's
 * shared edge anchoring (kitty_anchor.h) - which moves the whole row in ONE
 * DeferWindowPos transaction and then redraws the dialog and every child. A
 * private layout routine moving controls one at a time with MoveWindow is
 * what left the vacated pixels of one button sitting inside the next.
 * ------------------------------------------------------------------------ */

/* Where each control goes when the window grows. The list takes the slack in
 * both directions; the lines above it stay at the top and stretch; the
 * warning line and the buttons ride the bottom edge. */
static const struct kl_anchor kt_req_anchors[] = {
    {IDC_XFERREQ_INTRO,  KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_XFERREQ_COUNT,  KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_XFERREQ_LIST,
     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_XFERREQ_WARN,   KL_ANCH_LEFT | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_XFERREQ_LOCATE, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDYES,              KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDNO,               KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
};

/* The list may not be squeezed below this many rows. Three is enough to read
 * as a list and to scroll; the template's ten is a size, not a minimum. */
#define KT_REQ_MIN_ROWS 3

struct kt_req_dlg {
    struct kt_state *st;
    int ready;                          /* the baseline below has been captured */
    RECT rects[lenof(kt_req_anchors)];
    SIZE basesize, minsize;
};

/* The completion: everything kt_recv_ask used to do after the dialog. */
static void kt_recv_decide(struct kt_state *st, int allowed);

static void kt_size_str(uint64_t n, char *buf, size_t len)
{
    if (n < (1u << 10))
        snprintf(buf, len, KT_XFER5113_SIZE_B, (unsigned long long)n);
    else if (n < (1u << 20))
        snprintf(buf, len, KT_XFER5113_SIZE_KB, n / 1024.0);
    else if (n < (1u << 30))
        snprintf(buf, len, KT_XFER5113_SIZE_MB, n / (1024.0 * 1024.0));
    else
        snprintf(buf, len, KT_XFER5113_SIZE_GB, n / (1024.0 * 1024.0 * 1024.0));
}

static void kt_req_rect(HWND h, int id, RECT *rc)
{
    GetWindowRect(GetDlgItem(h, id), rc);
    MapWindowPoints(NULL, h, (POINT *)rc, 2);
}

/* The shared relayout, plus the one thing anchoring cannot do: the single
 * column always fills the list, whatever width it ended up with. */
static void kt_req_relayout(HWND h, struct kt_req_dlg *d)
{
    anchored_relayout(h, kt_req_anchors, lenof(kt_req_anchors), d->rects,
                      d->basesize);
    ListView_SetColumnWidth(GetDlgItem(h, IDC_XFERREQ_LIST), 0,
                            LVSCW_AUTOSIZE_USEHEADER);
}

/*
 * One row of the list, in pixels. Asked of the control, because a row is the
 * font's line plus whatever padding the list view itself adds - which is a
 * property of the control and the DPI, not something to guess. An empty list
 * has no row to measure, so the font's own line stands in.
 */
static int kt_req_row_height(HWND list)
{
    RECT r;
    HDC dc;
    int h = 0;

    if (ListView_GetItemCount(list) > 0 &&
        ListView_GetItemRect(list, 0, &r, LVIR_BOUNDS) && r.bottom > r.top)
        return r.bottom - r.top;
    dc = GetDC(list);
    if (dc) {
        HFONT font = (HFONT)SendMessage(list, WM_GETFONT, 0, 0);
        HFONT oldfont = font ? (HFONT)SelectObject(dc, font) : NULL;
        TEXTMETRICA tm;
        if (GetTextMetricsA(dc, &tm))
            h = tm.tmHeight + tm.tmExternalLeading + 2;
        if (oldfont)
            SelectObject(dc, oldfont);
        ReleaseDC(list, dc);
    }
    return h > 0 ? h : 16;
}

/*
 * How tall the window opens, and how short it may be made.
 *
 * Everything except the list is fixed: two lines above, the warning line and
 * the button row below, the margins and the frame. So both numbers are that
 * CHROME plus a number of rows - three at the minimum (the template's ten
 * rows are a size, not a floor, and pinning the minimum to the whole template
 * meant the window could never be made shorter than a ten-row list), and at
 * the opening size the rows actually there, never more than the template
 * asked for and never fewer than the minimum.
 */
static void kt_req_height(HWND h, struct kt_req_dlg *d)
{
    HWND list = GetDlgItem(h, IDC_XFERREQ_LIST);
    RECT lw, lc, wr;
    int row_h, frame_v, chrome_h, tmpl_rows, rows, want_h;

    if (!list || !GetWindowRect(list, &lw) || !GetClientRect(list, &lc) ||
        !GetWindowRect(h, &wr))
        return;
    row_h = kt_req_row_height(list);
    frame_v = (lw.bottom - lw.top) - (lc.bottom - lc.top);   /* its border */
    chrome_h = (wr.bottom - wr.top) - (lw.bottom - lw.top);
    tmpl_rows = (lw.bottom - lw.top - frame_v) / row_h;
    if (tmpl_rows < KT_REQ_MIN_ROWS)
        tmpl_rows = KT_REQ_MIN_ROWS;

    d->minsize.cy = chrome_h + frame_v + KT_REQ_MIN_ROWS * row_h;

    rows = ListView_GetItemCount(list);
    if (rows > tmpl_rows)
        rows = tmpl_rows;
    if (rows < KT_REQ_MIN_ROWS)
        rows = KT_REQ_MIN_ROWS;
    want_h = chrome_h + frame_v + rows * row_h;
    if (want_h != wr.bottom - wr.top)
        SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, want_h,
                     SWP_NOMOVE | SWP_NOZORDER);
}

/* What a row reads: the local path, or the name that was asked for with the
 * mark that nothing here answers to it. Wide, like every row of this list. */
static wchar_t *kt_req_row_text(const struct kt_entry *e)
{
    char *s;
    wchar_t *w;
    if (!e->missing)
        return kt_wdup(e->local);
    s = dupprintf(KT_XFER5113_REQ_NOT_FOUND, e->posix);
    w = kt_mb_to_wide(CP_UTF8, s, -1);
    sfree(s);
    return w ? w : kt_wdup(L"");
}

/* The count line, rebuilt: a located file changes both numbers. */
static void kt_req_count(HWND h, struct kt_req_dlg *d)
{
    struct kt_recv *r = d->st->recv;
    char size[64], *count;
    if (!r)
        return;
    kt_size_str(r->total_bytes, size, sizeof(size));
    count = dupprintf(KT_XFER5113_REQ_COUNT, r->nfiles, size);
    SetDlgItemTextA(h, IDC_XFERREQ_COUNT, count);
    sfree(count);
}

static struct kt_entry *kt_req_selected(HWND list, int *index)
{
    LVITEMA it;
    int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (i < 0)
        return NULL;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = i;
    if (!ListView_GetItem(list, &it) || !it.lParam)
        return NULL;
    if (index)
        *index = i;
    return (struct kt_entry *)it.lParam;
}

/* "Locate...": a local file in place of a requested name nothing here
 * answers to. The row becomes that file, checked, and its spec stops being
 * refused - but `posix` is left alone, so what leaves this PC still carries
 * the name the far end asked for. */
static void kt_req_locate(HWND h, struct kt_req_dlg *d)
{
    HWND list = GetDlgItem(h, IDC_XFERREQ_LIST);
    struct kt_recv *r = d->st->recv;
    struct kt_entry *e;
    struct kt_spec *sp;
    WIN32_FILE_ATTRIBUTE_DATA ad;
    char path[4096];
    wchar_t *w, *text;
    int i = -1;

    if (!r)
        return;
    e = kt_req_selected(list, &i);
    if (!e || !e->missing)
        return;
    path[0] = '\0';
    if (!OpenFileNameFrom(h, path, KT_XFER5113_REQ_LOCATE_TITLE,
                          KT_XFER5113_REQ_LOCATE_FILTER, NULL) || !path[0])
        return;
    w = kt_mb_to_wide(CP_ACP, path, -1);
    if (!w)
        return;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &ad) ||
        (ad.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        sfree(w);                       /* a folder or a link is not a file */
        return;
    }
    e->local = w;
    e->size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
    e->mtime_ns = kt_filetime_to_ns(&ad.ftLastWriteTime);
    e->readonly = (ad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    e->missing = 0;
    e->denied = 0;
    r->nfiles++;
    r->total_bytes += e->size;
    sp = kt_spec_by_fid(r, e->spec_fid);
    if (sp)
        sp->why = NULL;                 /* answered by a file now, not ENOENT */
    {
        char *a = kt_ansi(e->local);
        kt_log(d->st->term, dupprintf(KT_XFER5113_LOG_LOCATED, r->id, e->posix, a));
        sfree(a);
    }
    text = kt_req_row_text(e);
    if (text) {
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.iSubItem = 0;
        it.pszText = text;
        SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&it);
        sfree(text);
    }
    ListView_SetCheckState(list, i, TRUE);
    kt_req_count(h, d);
    EnableWindow(GetDlgItem(h, IDC_XFERREQ_LOCATE), FALSE);
}

/* The verdict, delivered after the window has gone - never from inside
 * WM_DESTROY, which also runs when the terminal is torn down under it. */
static void kt_req_finish(HWND h, int verdict)
{
    struct kt_req_dlg *d = (struct kt_req_dlg *)GetWindowLongPtr(h, GWLP_USERDATA);
    struct kt_state *st;

    if (!d)
        return;
    st = d->st;
    DestroyWindow(h);                   /* WM_DESTROY unregisters and frees d */
    kt_recv_decide(st, verdict);
}

static INT_PTR CALLBACK kt_req_dlgproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct kt_req_dlg *d = (struct kt_req_dlg *)GetWindowLongPtr(h, GWLP_USERDATA);
    switch (msg) {
      case WM_INITDIALOG: {
        RECT a, b, c, wr, cr;
        HWND list = GetDlgItem(h, IDC_XFERREQ_LIST);
        LVCOLUMNW col;
        struct kt_entry *e;
        HDWP dwp;
        SIZE ignore;                    /* anchored_capture's own minimum: the
                                         * one this dialog uses is computed in
                                         * kt_req_height, from a row count */
        int i = 0, margin, gap, btn_h, frame, need;
        int allow_w, deny_w, locate_w;

        d = (struct kt_req_dlg *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        /* The template's own spacing, in this monitor's pixels. */
        kt_req_rect(h, IDC_XFERREQ_INTRO, &a);
        kt_req_rect(h, IDC_XFERREQ_COUNT, &b);
        margin = a.left;
        gap = b.top - a.bottom;

        SetWindowTextA(h, KT_XFER5113_REQ_CAP);
        SetDlgItemTextA(h, IDC_XFERREQ_INTRO, KT_XFER5113_REQ_INTRO);
        SetDlgItemTextA(h, IDC_XFERREQ_WARN, KT_XFER5113_ASK_RECV_WARN);
        SetDlgItemTextA(h, IDYES, KT_XFER5113_REQ_BTN_ALLOW);
        SetDlgItemTextA(h, IDNO, KT_XFER5113_REQ_BTN_DENY);
        SetDlgItemTextA(h, IDC_XFERREQ_LOCATE, KT_XFER5113_REQ_BTN_LOCATE);
        kt_req_count(h, d);
        /* The warning line is painted by the theme engine, in the ink for the
         * theme in force - not by a colour written here (kitty_theme.h). */
        kitty_theme_mark_ink(GetDlgItem(h, IDC_XFERREQ_WARN), KITTY_INK_BAD);
        /* And the caption and taskbar icon of the terminal that raised it -
         * this session's own icon when it carries one. */
        kitty_dialog_icon(h, MainHwnd);

        /* AFTER the captions, and from the captions: the template's widths
         * are the widths ITS wordings needed, and are only a floor here. */
        kt_req_rect(h, IDYES, &a);
        kt_req_rect(h, IDNO, &b);
        kt_req_rect(h, IDC_XFERREQ_LOCATE, &c);
        btn_h = a.bottom - a.top;
        allow_w = kitty_theme_button_width(GetDlgItem(h, IDYES), a.right - a.left);
        deny_w = kitty_theme_button_width(GetDlgItem(h, IDNO), b.right - b.left);
        locate_w = kitty_theme_button_width(GetDlgItem(h, IDC_XFERREQ_LOCATE),
                                            c.right - c.left);

        /* Wider than the template allowed for: the window grows rather than a
         * caption spilling out of its button. */
        GetWindowRect(h, &wr);
        GetClientRect(h, &cr);
        frame = (wr.right - wr.left) - cr.right;
        need = 2 * margin + locate_w + gap + allow_w + gap + deny_w + frame;
        if (need > wr.right - wr.left) {
            SetWindowPos(h, NULL, 0, 0, need, wr.bottom - wr.top,
                         SWP_NOMOVE | SWP_NOZORDER);
            GetWindowRect(h, &wr);
            GetClientRect(h, &cr);
        }
        /* The narrowest it may become. The button row is the hard part, but
         * the template's own width is kept as the floor as well: it is what
         * holds the request line above the list unclipped. */
        d->minsize.cx = wr.right - wr.left;

        /* The three buttons at their measured widths, in ONE transaction:
         * Locate at the left, Allow and Deny at the right. */
        dwp = BeginDeferWindowPos(3);
        if (dwp)
            dwp = DeferWindowPos(dwp, GetDlgItem(h, IDC_XFERREQ_LOCATE), NULL,
                                 margin, c.top, locate_w, btn_h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp)
            dwp = DeferWindowPos(dwp, GetDlgItem(h, IDNO), NULL,
                                 cr.right - margin - deny_w, b.top, deny_w, btn_h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp)
            dwp = DeferWindowPos(dwp, GetDlgItem(h, IDYES), NULL,
                                 cr.right - margin - deny_w - gap - allow_w,
                                 a.top, allow_w, btn_h,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp)
            EndDeferWindowPos(dwp);

        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_WIDTH;
        col.cx = 100;
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        /* Wide inserts into a list view of an ANSI dialog: the control is
         * Unicode whatever its parent is, so every path shows as it is. */
        for (e = d->st->recv->entries; e; e = e->next) {
            LVITEMW it;
            wchar_t *text;
            if (e->is_dir)
                continue;
            text = kt_req_row_text(e);
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            it.pszText = text;
            it.lParam = (LPARAM)e;
            if (SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it) >= 0) {
                /* A row nothing answers to starts unchecked: there is no file
                 * to send until one is put behind it. */
                ListView_SetCheckState(list, i, e->missing ? FALSE : TRUE);
                i++;
            }
            sfree(text);
        }
        /*
         * The baseline every later resize is replayed against, captured at
         * the TEMPLATE's height with every control where the template put it
         * and at the width it has just been given. Anything that changes the
         * window after this point goes through the anchors, which is why it
         * comes before the height fit and not after it.
         */
        anchored_capture(h, kt_req_anchors, lenof(kt_req_anchors), d->rects,
                         &d->basesize, &ignore);
        d->ready = 1;
        /* The rows are in, so a row can be measured: open at the height they
         * need and pin the floor to a three-row list. Its SetWindowPos lands
         * as a WM_SIZE, which re-places everything against the baseline. */
        kt_req_height(h, d);
        ListView_SetColumnWidth(list, 0, LVSCW_AUTOSIZE_USEHEADER);
        kitty_centre_on_owner(h);
        SetFocus(GetDlgItem(h, IDNO));      /* Return means Deny */
        return FALSE;
      }
      case WM_SIZE:
        if (d && d->ready && wp != SIZE_MINIMIZED)
            kt_req_relayout(h, d);
        return TRUE;
      case WM_GETMINMAXINFO:
        if (d && d->ready) {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = d->minsize.cx;
            mmi->ptMinTrackSize.y = d->minsize.cy;
        }
        return TRUE;
      case WM_NOTIFY: {
        /* "Locate..." answers only for a row nothing is behind. */
        NMHDR *nm = (NMHDR *)lp;
        if (d && nm && nm->idFrom == IDC_XFERREQ_LIST &&
            nm->code == LVN_ITEMCHANGED) {
            struct kt_entry *e = kt_req_selected(nm->hwndFrom, NULL);
            EnableWindow(GetDlgItem(h, IDC_XFERREQ_LOCATE),
                         e && e->missing ? TRUE : FALSE);
        }
        return FALSE;
      }
      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_XFERREQ_LOCATE:
            if (!d)
                return TRUE;
            kt_req_locate(h, d);
            /* A cancel that arrived while the picker was up had its WM_CLOSE
             * refused (see WM_CLOSE below): act on it now. */
            if (d->st && d->st->recv &&
                (d->st->recv->cancel_pending || d->st->recv->abort_pending))
                kt_req_finish(h, 0);
            return TRUE;
          case IDYES: {
            /* Unchecked files stay listed and are refused when asked for.
             * Nothing checked is a Deny: no file may leave on that click. A
             * row nothing was put behind counts for nothing either way. */
            HWND list = GetDlgItem(h, IDC_XFERREQ_LIST);
            int n = ListView_GetItemCount(list), i, checked = 0;
            for (i = 0; i < n; i++) {
                LVITEMA it;
                struct kt_entry *e;
                memset(&it, 0, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = i;
                if (!ListView_GetItem(list, &it) || !it.lParam)
                    continue;
                e = (struct kt_entry *)it.lParam;
                if (e->missing)
                    e->denied = 1;
                else if (ListView_GetCheckState(list, i))
                    checked++;
                else
                    e->denied = 1;
            }
            kt_req_finish(h, checked > 0 ? 1 : 0);
            return TRUE;
          }
          case IDNO:
          case IDCANCEL:
            kt_req_finish(h, 0);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        /* Not while the "Locate..." picker is up: it owns this window, and
         * destroying the owner of a live common dialog frees state it is
         * still standing on. The picker's own return path closes instead. */
        if (!IsWindowEnabled(h))
            return TRUE;
        kt_req_finish(h, 0);            /* closing means Deny */
        return TRUE;
      case WM_DESTROY:
        ShinyRemoveAuxDialog(h);
        if (d) {
            if (d->st && d->st->recv && d->st->recv->dlg == h)
                d->st->recv->dlg = NULL;
            sfree(d);
        }
        SetWindowLongPtr(h, GWLP_USERDATA, 0);
        return FALSE;                   /* the manager tidies up as well */
    }
    return FALSE;
}

/* Put the dialog up. True when it stands and the answer will arrive in
 * kt_recv_decide(); false when the template did not load at all. */
static int kt_req_dialog_open(struct kt_state *st)
{
    struct kt_req_dlg *d = snew(struct kt_req_dlg);
    INITCOMMONCONTROLSEX icc;
    HWND h;

    memset(d, 0, sizeof(*d));
    d->st = st;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    h = kt_dialog_open(IDD_XFERREQ, kt_req_dlgproc, d);
    if (!h) {
        sfree(d);
        return 0;
    }
    st->recv->dlg = h;
    return 1;
}

/* The template did not load: the plain all-or-nothing question in the
 * suite's shared confirm box, which is modal by design. Never a silent
 * allow. 1 = allowed, 0 = refused. */
static int kt_req_fallback(struct kt_state *st)
{
    strbuf *list = strbuf_new();
    struct kt_entry *e;
    char *text;
    int i = 0, yes;

    for (e = st->recv->entries; e; e = e->next) {
        char *a;
        if (e->is_dir)
            continue;
        if (i++ >= KT_DIALOG_LINES) {
            put_fmt(list, KT_XFER5113_ASK_RECV_MORE, st->recv->nfiles - KT_DIALOG_LINES);
            break;
        }
        a = e->missing ? dupprintf(KT_XFER5113_REQ_NOT_FOUND, e->posix)
                       : kt_ansi(e->local);
        put_fmt(list, "%s\r\n", a);
        sfree(a);
    }
    text = dupprintf(KT_XFER5113_ASK_RECV, list->s);
    strbuf_free(list);
    yes = kt_ask(text, KT_XFER5113_ASK_RECV_WARN);
    sfree(text);
    return yes;
}

/* All specs are in: walk, then ask - always, whatever the permission
 * setting says. A setting that let the far end read local files without a
 * word would be a hole, so "never ask" applies to files ARRIVING only. */
static void kt_recv_ask(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;

    kt_recv_prepare(st);
    if (r->nentries == 0) {
        /* nothing a dialog could list: the refusals, then the session */
        kt_recv_send_spec_errors(st);
        kt_err(term, r, NULL, "ENOENT", KT_XFER5113_ST_NO_FILES);
        kt_recv_free(st);
        return;
    }
    r->in_dialog = 1;                   /* a command arriving now is held over */
    if (kt_req_dialog_open(st))
        return;                         /* answered in kt_recv_decide */
    kt_recv_decide(st, kt_req_fallback(st));
}

/* The decision, once the request dialog has gone. */
static void kt_recv_decide(struct kt_state *st, int allowed)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;

    if (!r)
        return;                         /* the session went while it stood */
    r->in_dialog = 0;
    r->dlg = NULL;
    if (st->dead) {
        kt_recv_free(st);
        kt_state_maybe_drop(st);
        return;
    }
    if (r->cancel_pending) {
        kt_ack(term, r, NULL, "CANCELED", NULL, -1);
        kt_log(term, dupprintf(KT_XFER5113_LOG_CANCELLED, r->id));
        kt_recv_free(st);
        return;
    }
    if (r->abort_pending) {
        kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, r->id));
        kt_recv_free(st);
        return;
    }
    if (!allowed) {
        kt_err(term, r, NULL, "EPERM", KT_XFER5113_ST_REFUSED);
        kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_DENIED, r->id));
        kt_recv_free(st);
        return;
    }
    r->accepted = 1;
    kt_ack(term, r, NULL, "OK", NULL, -1);
    kt_recv_list(st);
}

/*
 * The pump: stream the queued files one at a time, a chunk per turn, and stop
 * while the backend has more than KT_SENDBUF_HIGH queued - a large file must
 * not be turned into an equally large send buffer in one go. Re-armed by a
 * timer until the queue is empty.
 */
static void kt_pump(void *ctx, unsigned long now);

/* One stored deflate block per chunk: header (first time), then
 * BFINAL/BTYPE=00, LEN, NLEN, the bytes; at the end an empty final block and
 * the Adler-32. Valid RFC 1950, no compression - the client asked for zlib
 * and inflates whatever arrives, so the framing must be right. */
static size_t kt_zlib_wrap(struct kt_req *q, const unsigned char *in, size_t n,
                           int last, unsigned char *out)
{
    size_t o = 0;
    if (!q->header_sent) {
        out[o++] = 0x78; out[o++] = 0x01;
        q->header_sent = 1;
    }
    if (n > 0) {
        out[o++] = 0x00;                /* not final, stored */
        out[o++] = (unsigned char)(n & 0xff);
        out[o++] = (unsigned char)(n >> 8);
        out[o++] = (unsigned char)(~n & 0xff);
        out[o++] = (unsigned char)((~n >> 8) & 0xff);
        memcpy(out + o, in, n);
        o += n;
        q->adler = kt5113_adler32(q->adler, in, n);
    }
    if (last) {
        out[o++] = 0x01; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0xff; out[o++] = 0xff;
        out[o++] = (unsigned char)(q->adler >> 24);
        out[o++] = (unsigned char)(q->adler >> 16);
        out[o++] = (unsigned char)(q->adler >> 8);
        out[o++] = (unsigned char)q->adler;
    }
    return o;
}

static void kt_req_pop(struct kt_recv *r)
{
    struct kt_req *q = r->queue;
    r->queue = q->next;
    kt_req_free(q);
}

/* One chunk of the head request. */
static void kt_pump_one(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    struct kt_req *q = r->queue;
    unsigned char in[KT_CHUNK], out[KT_CHUNK + 32];
    DWORD want = q->zip ? KT_CHUNK_ZIP : KT_CHUNK, got = 0;
    int last;

    if (q->h == INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        q->h = CreateFileW(q->e->local, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (q->h == INVALID_HANDLE_VALUE || !GetFileSizeEx(q->h, &sz)) {
            char *a = kt_ansi(q->e->local);
            kt_err(term, r, q->fid, "EIO", KT_XFER5113_ST_READ_FAILED);
            kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_FAILED, r->id, a));
            sfree(a);
            kt_req_pop(r);
            return;
        }
        q->size = (uint64_t)sz.QuadPart;
    }
    if (!ReadFile(q->h, in, want, &got, NULL)) {
        char *a = kt_ansi(q->e->local);
        kt_err(term, r, q->fid, "EIO", KT_XFER5113_ST_READ_FAILED);
        kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_FAILED, r->id, a));
        sfree(a);
        kt_req_pop(r);
        return;
    }
    q->sent += got;
    last = got == 0 || q->sent >= q->size;
    if (q->zip) {
        size_t n = kt_zlib_wrap(q, in, got, last, out);
        kt_data(term, r->id, q->fid, out, n, last);
    } else {
        kt_data(term, r->id, q->fid, in, got, last);
    }
    if (last) {
        char *a = kt_ansi(q->e->local);
        r->nsent++;
        kt_log(term, dupprintf(KT_XFER5113_LOG_SENT, r->id, a, (unsigned long long)q->sent));
        sfree(a);
        kt_req_pop(r);
    }
}

static void kt_pump(void *ctx, unsigned long now)
{
    struct kt_state *st = (struct kt_state *)ctx;
    struct kt_recv *r = st->recv;
    int budget = 64;

    (void)now;
    if (!r || st->dead)
        return;
    r->timer_armed = 0;
    while (r->queue && budget-- > 0) {
        if (!st->term->backend) {
            kt_recv_free(st);           /* no connection left to send down */
            return;
        }
        if (backend_sendbuffer(st->term->backend) > KT_SENDBUF_HIGH)
            break;
        kt_pump_one(st);
    }
    if (r->queue && !r->timer_armed) {
        r->timer_armed = 1;
        schedule_timer(KT_PUMP_TICKS, kt_pump, st);
    }
}

/* action=file after the listing: a request for one file's data. Only a name
 * we listed is opened - looked up by exact string, never resolved again. */
static void kt_recv_request(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    struct kt_entry *e;
    struct kt_req *q, **pp;

    if (!c->fid[0])
        return;
    if (c->name_bad)
        goto not_listed;
    /* A row nothing was put behind was never listed, so a request naming it
     * is answered as unlisted - which is the ENOENT its spec already got. */
    for (e = r->entries; e; e = e->next)
        if (!e->is_dir && !e->missing && !strcmp(e->posix, c->name))
            break;
    if (!e) {
      not_listed:
        kt_err(term, r, c->fid, "ENOENT", KT_XFER5113_ST_NOT_LISTED);
        return;
    }
    if (e->denied) {
        /* left unchecked in the upload-request dialog: refused per file */
        kt_err(term, r, c->fid, "EPERM", KT_XFER5113_ST_DENIED);
        kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_SPEC, r->id, c->name, KT_XFER5113_ST_DENIED));
        return;
    }
    if (c->ttype == KT5113_TT_RSYNC) {
        /* the client would send a signature and expect a delta back */
        kt_err(term, r, c->fid, "EINVAL", KT_XFER5113_ST_RSYNC);
        kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_SPEC, r->id, c->name, KT_XFER5113_ST_RSYNC));
        return;
    }
    if (c->zip == KT5113_ZIP_UNKNOWN) {
        kt_err(term, r, c->fid, "EINVAL", KT_XFER5113_ST_BAD_ZIP);
        return;
    }
    q = snew(struct kt_req);
    memset(q, 0, sizeof(*q));
    strcpy(q->fid, c->fid);
    q->e = e;
    q->zip = c->zip == KT5113_ZIP_ZLIB;
    q->h = INVALID_HANDLE_VALUE;
    q->adler = 1;
    for (pp = &r->queue; *pp; pp = &(*pp)->next)
        ;
    *pp = q;
    if (!r->timer_armed)
        kt_pump(st, 0);
}

/* action=receive: a new session. Specs follow; the dialog waits for them. */
static void kt_recv_begin(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_recv *r;
    int n = c->has_size ? (int)(c->size > KT_MAX_FILES ? KT_MAX_FILES + 1 : c->size) : 0;

    if (st->recv) {
        if (!strcmp(st->recv->id, c->id)) {
            kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, c->id));
            kt_recv_free(st);
        } else {
            kt_status(term, c->quiet, 1, c->id, NULL, "EPERM", KT_XFER5113_ST_BUSY, NULL, -1);
            kt_log(term, dupprintf(KT_XFER5113_LOG_BUSY, c->id));
            return;
        }
    }
    if (n <= 0) {
        kt_status(term, c->quiet, 1, c->id, NULL, "EINVAL", KT_XFER5113_ST_NO_SPECS, NULL, -1);
        return;
    }
    if (n > KT_MAX_FILES) {
        kt_status(term, c->quiet, 1, c->id, NULL, "EINVAL", KT_XFER5113_ST_TOO_MANY, NULL, -1);
        return;
    }
    r = snew(struct kt_recv);
    memset(r, 0, sizeof(*r));
    strcpy(r->id, c->id);
    r->quiet = c->quiet;
    r->nspecs = n;
    r->specs = snewn((size_t)n, struct kt_spec);
    r->last = time(NULL);
    st->recv = r;
    kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_ASK, r->id, n));
}

static void kt_recv_cmd(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;

    if (r->in_dialog) {
        if (c->action == KT5113_AC_CANCEL)
            r->cancel_pending = 1;
        else
            r->abort_pending = 1;
        if (r->dlg)                     /* as in kt_send_cmd: take it down */
            PostMessage(r->dlg, WM_CLOSE, 0, 0);
        return;
    }
    r->last = time(NULL);
    switch (c->action) {
      case KT5113_AC_CANCEL:
        kt_ack(term, r, NULL, "CANCELED", NULL, -1);
        kt_log(term, dupprintf(KT_XFER5113_LOG_CANCELLED, r->id));
        kt_recv_free(st);
        break;
      case KT5113_AC_FINISH:
        kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_DONE, r->id, r->nsent));
        if (r->nsent > 0) {             /* [KiTTY] transfernotification */
            char folder[4096];          /* the click opens the upload folder */
            kitty_xfer_upload_dir(term->conf, folder, sizeof(folder));
            kitty_xfer_notify(KT_XFER_WHAT_KITTEN_SEND, 0, 0, folder);
        }
        kt_recv_free(st);
        break;
      case KT5113_AC_FILE:
        if (!r->accepted) {
            if (r->got < r->nspecs && c->fid[0] && !c->name_bad) {
                struct kt_spec *sp = &r->specs[r->got++];
                strcpy(sp->fid, c->fid);
                sp->name = dupstr(c->name);
            }
            if (r->got == r->nspecs)
                kt_recv_ask(st);
        } else if (r->listed) {
            kt_recv_request(st, c);
        }
        break;
      case KT5113_AC_DATA:
      case KT5113_AC_END_DATA:
        break;                          /* an rsync signature we refused: ignored */
      case KT5113_AC_RECEIVE:
        kt_log(term, dupprintf(KT_XFER5113_LOG_DROPPED, r->id));
        kt_recv_free(st);
        break;
      default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------ */

static void kt_prune(struct kt_state *st)
{
    time_t now = time(NULL);
    if (st->send && !st->send->in_dialog && now - st->send->last > KT_EXPIRE_SECONDS) {
        kt_log(st->term, dupprintf(KT_XFER5113_LOG_EXPIRED, st->send->id, KT_EXPIRE_SECONDS / 60));
        kt_send_free(st);
    }
    if (st->recv && !st->recv->in_dialog && now - st->recv->last > KT_EXPIRE_SECONDS) {
        kt_log(st->term, dupprintf(KT_XFER5113_LOG_EXPIRED, st->recv->id, KT_EXPIRE_SECONDS / 60));
        kt_recv_free(st);
    }
}

void kitty_transfer_osc(Terminal *term)
{
    struct kt_state *st;
    kt5113_cmd *c;

    if (!term || !term->conf)
        return;
    st = kt_state_get(term, 1);
    if (term->osc_str_overflow) {
        /* Truncated: its data cannot be trusted and its id may be gone. */
        if (!st->oversize_logged) {
            st->oversize_logged = 1;
            kt_log(term, dupstr(KT_XFER5113_LOG_OVERSIZE));
        }
        return;
    }
    c = snew(kt5113_cmd);
    kt5113_parse(term->osc_string, (size_t)term->osc_strlen, c);
    if (c->bad || !c->id[0] || c->action == KT5113_AC_NONE ||
        c->action == KT5113_AC_UNKNOWN) {
        sfree(c);
        return;
    }
    kt_prune(st);

    if (st->send && !strcmp(st->send->id, c->id) && c->action != KT5113_AC_RECEIVE)
        kt_send_cmd(st, c);
    else if (st->recv && !strcmp(st->recv->id, c->id) && c->action != KT5113_AC_SEND)
        kt_recv_cmd(st, c);
    else if (c->action == KT5113_AC_SEND)
        kt_send_begin(st, c);
    else if (c->action == KT5113_AC_RECEIVE)
        kt_recv_begin(st, c);
    /* anything else names a session we do not have: ignored, like the
     * reference implementation does */
    sfree(c);
}

void kitty_transfer_free(Terminal *term)
{
    struct kt_state *st = kt_state_get(term, 0);
    HWND a, b;

    if (!st)
        return;
    /*
     * The terminal is going. A request dialog of ours may still be standing:
     * destroying it is synchronous, so by the time DestroyWindow returns the
     * window is gone and its session half can be freed here - the completion
     * is deliberately not called from WM_DESTROY, and `dead` makes the
     * question a Deny for anything that does reach one. `freeing` keeps a
     * completion from dropping the node while this function still holds it.
     */
    st->dead = 1;
    st->freeing = 1;
    a = st->send ? st->send->dlg : NULL;
    b = st->recv ? st->recv->dlg : NULL;
    if (a)
        DestroyWindow(a);
    if (b)
        DestroyWindow(b);
    kt_send_free(st);
    kt_recv_free(st);
    kt_state_drop(st);
}
