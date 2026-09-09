/*
 * kitty_transfer.c - kitty's file-transfer protocol (OSC 5113): the terminal
 * half, so that `kitten transfer` on the far end can move files through the
 * terminal byte stream itself - across nested ssh hops, telnet, a serial line.
 *
 * Spec: docs/file-transfer-protocol.rst in the kitty repository. The wire
 * parser and the name rules are in kitty_transfer.h (shared with the unit
 * test); this file holds the sessions, the Win32 file work, the permission
 * dialog and the replies.
 *
 * Two kinds of session, one of each at a time per terminal:
 *
 *   send    (far end -> this PC)  send / file... / data... / end_data / finish
 *   receive (this PC -> far end)  receive+specs / [dialog] / listing /
 *                                 file requests / data... / finished
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

#include "kitty.h"               /* kitty_xfer_download_dir */
#include "kitty_win.h"           /* OpenDirNameFrom */
#include "kitty_transfer_text.h"
#define KT5113_PARSE_IMPL
#include "kitty_transfer.h"

extern HWND MainHwnd;            /* kitty.c: the terminal window */
/* The suite's themed Yes/No box (kitty_win.c): true when Yes was pressed. */
int kitty_confirm_box(HWND owner, const char *caption, const char *text,
                      const char *warn_red);
/* The reply channel (kitty_osc52.c): a complete sequence to the backend. */
void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len);

#define KT_EXPIRE_SECONDS   (10 * 60)   /* idle session, as the reference does */
#define KT_MAX_FILES        8192        /* per session, either direction */
#define KT_FILE_CEILING     ((uint64_t)4 << 30)
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
    wchar_t *dest;
    time_t last;
    int nfiles, ndone;
    struct kt_file *files;
    struct kt_dir *dirs;
};

/* One entry of the listing sent to the far end in a receive session. Data
 * requests are honoured only for names in this list. */
struct kt_entry {
    char rid[24];                       /* our id for it: the st= of the listing */
    char *posix;
    wchar_t *local;
    int is_dir;
    uint64_t size;
    struct kt_entry *next;
};

struct kt_spec {
    char fid[KT5113_ID_MAX + 1];
    char *name;
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
    int nspecs, got;
    struct kt_spec *specs;
    struct kt_entry *entries;
    int nentries;
    uint64_t rid_counter;
    struct kt_req *queue;
    int timer_armed;
    time_t last;
    int nsent;
};

/* Per-terminal state. Kept outside the Terminal struct (terminal.h is the
 * cross-platform file) in a small list keyed by the pointer. A node is never
 * freed while one of its dialogs is on the stack: term_free during a modal
 * dialog marks it dead and the dialog path finishes the job. */
struct kt_state {
    Terminal *term;
    struct kt_send *send;
    struct kt_recv *recv;
    int latched;                        /* policy 1: a grant was given */
    int oversize_logged;
    int dead;
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

/* ------------------------------------------------------------------------
 * Small helpers: strings, paths, logging
 * ------------------------------------------------------------------------ */

static void kt_log(Terminal *term, char *msg)
{
    logevent(term->logctx, msg);
    sfree(msg);
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
        if (f->written + chunk > KT_FILE_CEILING) {
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

/* action=send: a new session. The dialog (and the folder picker) run here,
 * before the OK that lets the client continue. */
static void kt_send_begin(struct kt_state *st, const kt5113_cmd *c)
{
    Terminal *term = st->term;
    struct kt_send *s;
    char folder[4096], *a;
    int policy, allowed;

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
    st->send = s;                       /* registered before the dialog: commands
                                         * arriving meanwhile must find it */

    kitty_xfer_download_dir(term->conf, folder, sizeof(folder));
    policy = conf_get_int(term->conf, CONF_xfer_permission);
    if (policy == 2 || (policy == 1 && st->latched)) {
        allowed = 1;
        kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_AUTO, s->id));
    } else {
        char *text = dupprintf(KT_XFER5113_ASK_SEND, folder);
        kt_log(term, dupprintf(KT_XFER5113_LOG_SEND_ASK, s->id, folder));
        s->in_dialog = 1;
        allowed = kt_ask(text, KT_XFER5113_ASK_SEND_WARN);
        s->in_dialog = 0;
        sfree(text);
        if (allowed && policy == 1)
            st->latched = 1;
    }
    if (st->dead) {                     /* the window went away under the dialog */
        kt_send_free(st);
        kt_state_drop(st);
        return;
    }
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
    if (conf_get_bool(term->conf, CONF_xfer_ask_destination)) {
        char picked[4096];
        int ok;
        s->in_dialog = 1;
        ok = OpenDirNameFrom(MainHwnd, picked, folder, KT_XFER5113_PICK_TITLE);
        s->in_dialog = 0;
        if (st->dead) {
            kt_send_free(st);
            kt_state_drop(st);
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
        strncpy(folder, picked, sizeof(folder) - 1);
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
        /* Re-entered from the dialog's message loop. The spec: a command
         * before OK drops the session; a cancel is answered afterwards. */
        if (c->action == KT5113_AC_CANCEL)
            s->cancel_pending = 1;
        else
            s->abort_pending = 1;
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
 * would ever open: everything is taken relative to the download folder except
 * the spec's own /C:/... form, which names a drive path outright - and the
 * dialog shows exactly what was resolved. */
static wchar_t *kt_resolve_spec(Terminal *term, const char *name)
{
    const char *rel;
    size_t rn;
    char drive;
    int kind = kt5113_spec_classify(name, strlen(name), &rel, &rn, &drive);
    wchar_t *base, *path, *w;

    if (kind == KT5113_SPEC_BAD)
        return NULL;
    if (kind == KT5113_SPEC_DRIVE) {
        wchar_t root[4] = { (wchar_t)drive, L':', L'\\', 0 };
        base = kt_wdup(root);
    } else {
        char folder[4096];
        kitty_xfer_download_dir(term->conf, folder, sizeof(folder));
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
                                     int is_dir, uint64_t size)
{
    struct kt_entry *e = snew(struct kt_entry), **pp;
    memset(e, 0, sizeof(*e));
    snprintf(e->rid, sizeof(e->rid), "%llu", (unsigned long long)++r->rid_counter);
    e->local = kt_wdup(local);
    e->posix = kt_posix(local);
    e->is_dir = is_dir;
    e->size = size;
    for (pp = &r->entries; *pp; pp = &(*pp)->next)
        ;
    *pp = e;
    r->nentries++;
    return e;
}

/* List one path (recursively for a folder), sending an entry per item.
 * Reparse points - symlinks, junctions - are skipped and never followed.
 * Returns 0 when the per-session ceiling was hit. */
static int kt_list_dir(struct kt_state *st, const char *spec_fid,
                       const wchar_t *dir, const char *parent_rid, int depth)
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
            e = kt_entry_add(r, child, 1, 0);
            kt_entry_reply(st->term, r, spec_fid, e,
                           kt_filetime_to_ns(&fd.ftLastWriteTime), 0, parent_rid);
            if (depth < KT_MAX_DEPTH && !kt_list_dir(st, spec_fid, child, e->rid, depth + 1)) {
                sfree(child);
                FindClose(h);
                return 0;
            }
        } else {
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e = kt_entry_add(r, child, 0, sz);
            kt_entry_reply(st->term, r, spec_fid, e,
                           kt_filetime_to_ns(&fd.ftLastWriteTime),
                           (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0,
                           parent_rid);
        }
        sfree(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* After permission: the metadata of everything asked for, then OK with the
 * home folder, as the spec's receive flow prescribes. */
static void kt_recv_list(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    int i, stop = 0;

    for (i = 0; i < r->got && !stop; i++) {
        const struct kt_spec *sp = &r->specs[i];
        wchar_t *local = kt_resolve_spec(term, sp->name);
        DWORD attrs = 0;
        WIN32_FILE_ATTRIBUTE_DATA ad;
        const char *why = NULL, *code = "EINVAL";

        if (!local) {
            why = KT_XFER5113_ST_OUTSIDE;
        } else if (!GetFileAttributesExW(local, GetFileExInfoStandard, &ad)) {
            why = KT_XFER5113_ST_NOT_FOUND; code = "ENOENT";
        } else if ((attrs = ad.dwFileAttributes) & FILE_ATTRIBUTE_REPARSE_POINT) {
            why = KT_XFER5113_ST_LINKS; code = "ENOTSUP";
        } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            struct kt_entry *e = kt_entry_add(r, local, 1, 0);
            kt_entry_reply(term, r, sp->fid, e, kt_filetime_to_ns(&ad.ftLastWriteTime), 0, NULL);
            if (!kt_list_dir(st, sp->fid, local, e->rid, 1)) {
                why = KT_XFER5113_ST_TOO_MANY;
                stop = 1;
            }
        } else {
            uint64_t sz = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            struct kt_entry *e = kt_entry_add(r, local, 0, sz);
            kt_entry_reply(term, r, sp->fid, e, kt_filetime_to_ns(&ad.ftLastWriteTime),
                           (attrs & FILE_ATTRIBUTE_READONLY) != 0, NULL);
        }
        if (why) {
            kt_err(term, r, sp->fid, code, why);
            kt_log(term, dupprintf(KT_XFER5113_LOG_RECV_SPEC, r->id, sp->name, why));
        }
        sfree(local);
    }
    if (r->nentries == 0) {
        kt_err(term, r, NULL, "ENOENT", KT_XFER5113_ST_NO_FILES);
        kt_recv_free(st);
        return;
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

/* All specs are in: ask, always - whatever the permission setting says. A
 * setting that let the far end read local files without a word would be a
 * hole, so "never ask" applies to files ARRIVING only. */
static void kt_recv_ask(struct kt_state *st)
{
    Terminal *term = st->term;
    struct kt_recv *r = st->recv;
    strbuf *list = strbuf_new();
    char *text;
    int i, allowed;

    for (i = 0; i < r->got; i++) {
        wchar_t *local;
        if (i >= KT_DIALOG_LINES) {
            put_fmt(list, KT_XFER5113_ASK_RECV_MORE, r->got - i);
            break;
        }
        local = kt_resolve_spec(term, r->specs[i].name);
        if (local) {
            char *a = kt_ansi(local);
            put_fmt(list, "%s\r\n", a);
            sfree(a);
            sfree(local);
        } else {
            put_fmt(list, KT_XFER5113_ASK_RECV_BAD "\r\n", r->specs[i].name);
        }
    }
    text = dupprintf(KT_XFER5113_ASK_RECV, list->s);
    strbuf_free(list);
    r->in_dialog = 1;
    allowed = kt_ask(text, KT_XFER5113_ASK_RECV_WARN);
    r->in_dialog = 0;
    sfree(text);
    if (st->dead) {
        kt_recv_free(st);
        kt_state_drop(st);
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
    for (e = r->entries; e; e = e->next)
        if (!e->is_dir && !strcmp(e->posix, c->name))
            break;
    if (!e) {
      not_listed:
        kt_err(term, r, c->fid, "ENOENT", KT_XFER5113_ST_NOT_LISTED);
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
    if (!st)
        return;
    if ((st->send && st->send->in_dialog) || (st->recv && st->recv->in_dialog)) {
        /* A modal dialog of ours is on the stack. Whatever is not under it
         * goes now; the dialog path frees the rest when it returns. */
        st->dead = 1;
        if (st->send && !st->send->in_dialog)
            kt_send_free(st);
        if (st->recv && !st->recv->in_dialog)
            kt_recv_free(st);
        return;
    }
    kt_send_free(st);
    kt_recv_free(st);
    kt_state_drop(st);
}
