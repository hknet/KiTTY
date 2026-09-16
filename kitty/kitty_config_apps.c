/*
 * kitty_config_apps.c - Application > Security > Applications: the KiTTY++
 * program files that travel with this installation, each checked against
 * what it carries - the release stamp (kitty_selfcheck.c) and the
 * Authenticode signature (kitty_renameguard.c's reading) - as far as this
 * build can judge, with a detail box for the selected file, a banner, Copy
 * and Verify again.
 *
 * THE RULE is kitty_verify_sibling()'s, extended to the stamp: the legs the
 * RUNNING binary carries are required of every known sibling. A required
 * leg that is missing or fails is a WARNING; a leg the running binary does
 * not carry is judged only when the sibling has it, and then a failure is a
 * WARNING too; a file version that differs from the running binary's is a
 * WARNING (not from this release); "authenticated" when every required leg
 * says ours and no other leg fails; "unverified" only when no leg could be
 * judged at all. So a release file with its signature stripped and a valid
 * stamp is red (the running release carries a signature), an unsigned and
 * unstamped test build checking its own set is unverified throughout, and a
 * 64-bit release on a Windows that cannot judge SHA-2 is unverified.
 *
 * Only the files we KNOW are verified (the ship list below); any other .exe
 * in the folder is counted and named, never judged - a stranger is never
 * silent, and we make no claim about it. DLLs are left out: kitty.dll is the
 * optional icon library, and a verdict on an unknown DLL says nothing.
 *
 * The check runs one file per tick of a zero-delay timer on the panel's first
 * refresh, so the rows appear as they are judged and the box never freezes
 * (hashing ten files takes seconds on old hardware). Every WARNING row is
 * also one Application event-log line, once per process per file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "dialog.h"
#include <windows.h>
#include "kitty_config_int.h"
#include "kitty_text.h"          /* KT_APPS_* */
#include "kitty_renameguard.h"   /* kitty_signature_reading, kitty_file_has_cert_table, kitty_guard_report */
#include "kitty_selfcheck.h"     /* kitty_selfcheck_file */
#include "kitty_authenticode.h"  /* kitty_file_version */
#include "kitty_winutil.h"       /* SetTextToClipboard */
#include "kitty_gui.h"           /* kitty_cfg_ctrl_hwnd */

/* The program files a release ships, both flavours (the MSI set and the
 * portable ZIP's kitty_portable.exe). This is a second copy of the MSI
 * component list and of the packager's rename map; the release preflight
 * compares them (see the design note), because such lists fail silently. */
static const char *const apps_known[] = {
    "kitty.exe", "kitty_portable.exe", "klink.exe", "kscp.exe", "ksftp.exe",
    "kageant.exe", "kittygen.exe", "kittygen-cli.exe", "kitty_pterm.exe",
    "kitty_tel.exe",
};

enum { APPS_PENDING = -1 };
enum { APPS_V_PENDING = 0, APPS_V_AUTH, APPS_V_WARN, APPS_V_UNVERIFIED };

struct apps_row {
    char name[64];
    char path[MAX_PATH];
    int has_version;
    unsigned long vms, vls;
    int sig;                     /* KG_SIG_* or APPS_PENDING */
    char signer[256];
    int stamp;                   /* kitty_selfcheck_file's return or APPS_PENDING */
    char stamp_reason[32];
    char sha[65];
    int verdict;                 /* APPS_V_* */
    char basis[160];             /* what the verdict rests on, for the detail box */
};

struct apps_data {
    dlgparam *dlg;
    dlgcontrol *listbox, *detail, *banner, *verify, *copy, *note;
    char dir[MAX_PATH];
    char own[MAX_PATH];
    int own_sig_carried;         /* the running file has a certificate table */
    int own_stamp_carried;       /* the running file's stamp verifies with this build's key */
    int stamp_key;               /* this build carries a key at all */
    int own_has_version;
    unsigned long own_vms, own_vls;
    struct apps_row *rows;
    int nrows;
    int next;                    /* the row the next tick judges */
    UINT_PTR timer;
    char **strangers;
    int nstrangers;
    void (*prev_closing)(void);  /* the closing hook that was there before ours */
};
static struct apps_data *kitty_apps_active;

dlgcontrol *kitty_apps_fill_ctrl(void)
{
    return (kitty_apps_active && kitty_apps_active->listbox) ?
        kitty_apps_active->listbox : NULL;
}

/* ---- the scan --------------------------------------------------------------- */

static int apps_is_known(const char *name)
{
    size_t i;
    for (i = 0; i < lenof(apps_known); i++)
        if (!_stricmp(apps_known[i], name)) return 1;
    return 0;
}

static void apps_free_rows(struct apps_data *a)
{
    int i;
    sfree(a->rows); a->rows = NULL; a->nrows = 0;
    for (i = 0; i < a->nstrangers; i++) sfree(a->strangers[i]);
    sfree(a->strangers); a->strangers = NULL; a->nstrangers = 0;
}

/* The folder's rows: the known names that are present, in ship-list order,
 * and the count of every other .exe. The running file's own legs decide
 * what is required of the others. */
static void apps_scan(struct apps_data *a)
{
    size_t i;
    DWORD n;
    char pattern[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE hf;
    char reason[32];

    apps_free_rows(a);
    n = GetModuleFileNameA(NULL, a->own, sizeof(a->own));
    if (n == 0 || n >= sizeof(a->own)) { a->own[0] = '\0'; a->dir[0] = '\0'; return; }
    strcpy(a->dir, a->own);
    { char *p = strrchr(a->dir, '\\'); if (p) *p = '\0'; }

    /* what THIS file carries */
    a->own_sig_carried = kitty_file_has_cert_table(a->own) == 1;
    switch (kitty_selfcheck_file(a->own, reason, sizeof(reason), NULL, 0)) {
      case 0: a->stamp_key = 1; a->own_stamp_carried = 1; break;
      case 3: a->stamp_key = 0; a->own_stamp_carried = 0; break;
      default: a->stamp_key = 1; a->own_stamp_carried = 0; break;
    }
    a->own_has_version = kitty_file_version(a->own, &a->own_vms, &a->own_vls);

    a->rows = snewn(lenof(apps_known), struct apps_row);
    for (i = 0; i < lenof(apps_known); i++) {
        struct apps_row *r = &a->rows[a->nrows];
        memset(r, 0, sizeof(*r));
        snprintf(r->path, sizeof(r->path), "%s\\%s", a->dir, apps_known[i]);
        if (GetFileAttributesA(r->path) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(r->name, sizeof(r->name), "%s", apps_known[i]);
        r->sig = APPS_PENDING; r->stamp = APPS_PENDING; r->verdict = APPS_V_PENDING;
        a->nrows++;
    }

    snprintf(pattern, sizeof(pattern), "%s\\*.exe", a->dir);
    hf = FindFirstFileA(pattern, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (apps_is_known(fd.cFileName)) continue;
            a->strangers = sresize(a->strangers, a->nstrangers + 1, char *);
            a->strangers[a->nstrangers++] = dupstr(fd.cFileName);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
}

/* ---- the judgement ---------------------------------------------------------- */

static const char *apps_sig_text(const struct apps_row *r)
{
    switch (r->sig) {
      case KG_SIG_OURS:     return KT_APPS_SIG_OURS;
      case KG_SIG_MODIFIED: return KT_APPS_SIG_MODIFIED;
      case KG_SIG_UNSIGNED: return KT_APPS_SIG_UNSIGNED;
      case KG_SIG_OTHER:    return KT_APPS_SIG_OTHER;
      case KG_SIG_CANNOT:   return KT_APPS_SIG_CANNOT;
      default:              return "";
    }
}

static const char *apps_stamp_text(const struct apps_data *a, const struct apps_row *r)
{
    if (r->stamp == APPS_PENDING) return "";
    if (!a->stamp_key || r->stamp == 3) return KT_APPS_STAMP_NOKEY;
    if (r->stamp == 0) return KT_APPS_STAMP_VALID;
    if (r->stamp == 2) return KT_APPS_STAMP_CANNOT;
    if (!strcmp(r->stamp_reason, "no stamp")) return KT_APPS_STAMP_NONE;
    if (!strcmp(r->stamp_reason, "modified") || !strcmp(r->stamp_reason, "truncated")) return KT_APPS_STAMP_MODIFIED;
    return KT_APPS_STAMP_OTHER_RELEASE;   /* "bad stamp": another release's key, or a corrupt stamp */
}

static const char *apps_verdict_text(const struct apps_row *r)
{
    switch (r->verdict) {
      case APPS_V_AUTH:       return KT_APPS_V_AUTH;
      case APPS_V_WARN:       return KT_APPS_V_WARN;
      case APPS_V_UNVERIFIED: return KT_APPS_V_UNVERIFIED;
      default:                return "";
    }
}

static void apps_version_text(const struct apps_row *r, char *buf, size_t n)
{
    if (r->has_version)
        snprintf(buf, n, "%lu.%lu.%lu.%lu", r->vms >> 16, r->vms & 0xffff, r->vls >> 16, r->vls & 0xffff);
    else
        snprintf(buf, n, "-");
}

/* One file, both legs, then the rule above. */
static void apps_judge(struct apps_data *a, struct apps_row *r)
{
    int ok = 0, fail = 0;
    const char *why = "";

    r->has_version = kitty_file_version(r->path, &r->vms, &r->vls);
    r->sig = kitty_signature_reading(r->path, r->signer, sizeof(r->signer));
    r->stamp = kitty_selfcheck_file(r->path, r->stamp_reason, sizeof(r->stamp_reason), r->sha, sizeof(r->sha));

    /* the signature leg */
    switch (r->sig) {
      case KG_SIG_OURS:     ok++; break;
      case KG_SIG_MODIFIED: fail++; why = KT_APPS_WHY_SIG_MODIFIED; break;
      case KG_SIG_OTHER:    fail++; why = KT_APPS_WHY_SIG_OTHER; break;
      case KG_SIG_UNSIGNED: if (a->own_sig_carried) { fail++; why = KT_APPS_WHY_SIG_MISSING; } break;
      default: break;                              /* cannot judge: no vote */
    }
    /* the stamp leg */
    if (a->stamp_key && r->stamp != 3) {
        if (r->stamp == 0) ok++;
        else if (r->stamp == 1) {
            if (!strcmp(r->stamp_reason, "no stamp")) {
                if (a->own_stamp_carried) { fail++; if (!*why) why = KT_APPS_WHY_STAMP_MISSING; }
            } else { fail++; if (!*why) why = KT_APPS_WHY_STAMP_FAILED; }
        }
        /* 2 = could not read: no vote */
    }
    /* the version: a known file from another release travels with us by mistake */
    if (a->own_has_version && r->has_version && (r->vms != a->own_vms || r->vls != a->own_vls)) {
        fail++; if (!*why) why = KT_APPS_WHY_VERSION;
    }

    r->verdict = fail ? APPS_V_WARN : ok ? APPS_V_AUTH : APPS_V_UNVERIFIED;
    if (fail) snprintf(r->basis, sizeof(r->basis), "%s", why);
    else if (ok) snprintf(r->basis, sizeof(r->basis), KT_APPS_WHY_OK,
                          a->own_sig_carried && a->own_stamp_carried ? KT_APPS_REQ_BOTH :
                          a->own_sig_carried ? KT_APPS_REQ_SIG :
                          a->own_stamp_carried ? KT_APPS_REQ_STAMP : KT_APPS_REQ_NONE);
    else snprintf(r->basis, sizeof(r->basis), "%s", KT_APPS_WHY_UNVERIFIED);
}

/* One Application event-log line per WARNING file, once per process. */
static void apps_log_warning(const struct apps_row *r)
{
    static char *seen[64];
    static int nseen;
    int i;
    char msg[600];
    for (i = 0; i < nseen; i++) if (!strcmp(seen[i], r->path)) return;
    if (nseen < (int)lenof(seen)) seen[nseen++] = dupstr(r->path);
    snprintf(msg, sizeof(msg), KT_APPS_EVENTLOG, r->name, r->basis);
    kitty_guard_report(msg, 1, 0);
}

/* ---- the list ---------------------------------------------------------------- */

static char *apps_row_text(const struct apps_data *a, const struct apps_row *r)
{
    char ver[48];
    apps_version_text(r, ver, sizeof(ver));
    return dupprintf("%s\t%s\t%s\t%s\t%s", r->name, ver, apps_sig_text(r), apps_stamp_text(a, r), apps_verdict_text(r));
}

static bool apps_row_ink(dlgcontrol *ctrl, int id, bool dark, COLORREF *ink)
{
    struct apps_data *a = (struct apps_data *)ctrl->context.p;
    if (!a || id < 0 || id >= a->nrows) return false;
    if (a->rows[id].verdict == APPS_V_WARN) { *ink = dark ? RGB(255, 110, 110) : RGB(192, 0, 0); return true; }
    if (a->rows[id].verdict == APPS_V_UNVERIFIED) { *ink = dark ? RGB(160, 160, 160) : RGB(110, 110, 110); return true; }
    return false;
}

static void apps_fill(struct apps_data *a, dlgparam *dlg)
{
    int i;
    dlg_update_start(a->listbox, dlg);
    dlg_listbox_clear(a->listbox, dlg);
    dlg_listbox_addwithid(a->listbox, dlg, KT_APPS_COL_HEAD, -1);
    for (i = 0; i < a->nrows; i++) {
        char *row = apps_row_text(a, &a->rows[i]);
        dlg_listbox_addwithid(a->listbox, dlg, row, i);
        sfree(row);
    }
    dlg_update_done(a->listbox, dlg);
}

/* Rewrite one row in place (a verdict came in) - the selection stays. */
static void apps_update_row(struct apps_data *a, int idx)
{
    HWND h = kitty_cfg_ctrl_hwnd(a->listbox);
    int n, r;
    if (!h || idx < 0 || idx >= a->nrows) return;
    n = (int)SendMessage(h, LB_GETCOUNT, 0, 0);
    for (r = 1; r < n; r++) {
        if ((int)SendMessage(h, LB_GETITEMDATA, r, 0) == idx) {
            char *row = apps_row_text(a, &a->rows[idx]);
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

static int apps_selected(struct apps_data *a, dlgparam *dlg)
{
    int i, n = 0, id = -1;
    for (i = 1; i <= a->nrows; i++)
        if (dlg_listbox_issel(a->listbox, dlg, i)) { id = dlg_listbox_getid(a->listbox, dlg, i); n++; }
    return n == 1 ? id : -1;
}

/* The detail box: the selected file in full, the strangers when the header
 * row is selected, the note otherwise. */
static void apps_show_detail(struct apps_data *a, dlgparam *dlg, int idx)
{
    strbuf *sb = strbuf_new();
    if (idx >= 0 && idx < a->nrows) {
        const struct apps_row *r = &a->rows[idx];
        char ver[48];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        apps_version_text(r, ver, sizeof(ver));
        put_fmt(sb, KT_APPS_DETAIL_FILE, r->path);
        if (GetFileAttributesExA(r->path, GetFileExInfoStandard, &fa))
            put_fmt(sb, "\r\n" KT_APPS_DETAIL_SIZE, (unsigned long)(((unsigned __int64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow));
        put_fmt(sb, "\r\n" KT_APPS_DETAIL_VERSION, ver);
        if (r->verdict != APPS_V_PENDING) {
            put_fmt(sb, "\r\n" KT_APPS_DETAIL_SIGNATURE, apps_sig_text(r), r->signer[0] ? r->signer : "-");
            put_fmt(sb, "\r\n" KT_APPS_DETAIL_STAMP, apps_stamp_text(a, r), r->sha[0] ? r->sha : "-");
            put_fmt(sb, "\r\n" KT_APPS_DETAIL_VERDICT, apps_verdict_text(r), r->basis);
        } else
            put_fmt(sb, "\r\n%s", KT_APPS_DETAIL_PENDING);
    } else if (a->nstrangers) {
        int i;
        put_fmt(sb, KT_APPS_DETAIL_STRANGERS, a->nstrangers);
        for (i = 0; i < a->nstrangers; i++) put_fmt(sb, "\r\n  %s", a->strangers[i]);
    } else
        put_dataz(sb, KT_APPS_DETAIL_NONE);
    dlg_editbox_set(a->detail, dlg, sb->s);
    strbuf_free(sb);
}

static void apps_banner(struct apps_data *a, dlgparam *dlg)
{
    strbuf *sb = strbuf_new();
    int i, warn = 0, unv = 0;
    for (i = 0; i < a->nrows; i++) {
        if (a->rows[i].verdict == APPS_V_WARN) warn++;
        else if (a->rows[i].verdict == APPS_V_UNVERIFIED) unv++;
    }
    if (a->next < a->nrows)
        put_fmt(sb, KT_APPS_BANNER_CHECKING, a->next, a->nrows);
    else if (warn)
        put_fmt(sb, KT_APPS_BANNER_WARN, warn, a->nrows);
    else if (unv == a->nrows && a->nrows && !a->own_sig_carried && !a->own_stamp_carried)
        put_fmt(sb, KT_APPS_BANNER_NOTHING, a->nrows);
    else if (unv)
        put_fmt(sb, KT_APPS_BANNER_CANNOT, a->nrows, unv);
    else
        put_fmt(sb, KT_APPS_BANNER_OK, a->nrows);
    if (a->nstrangers)
        put_fmt(sb, " " KT_APPS_BANNER_STRANGERS, a->nstrangers);
    dlg_label_change(a->banner, dlg, sb->s);
    strbuf_free(sb);
}

/* ---- the run: one file per tick -------------------------------------------- */

static void apps_run_stop(struct apps_data *a)
{
    if (a->timer) { KillTimer(NULL, a->timer); a->timer = 0; }
}

static void CALLBACK apps_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    struct apps_data *a = kitty_apps_active;
    (void)hwnd; (void)msg; (void)now;
    if (!a || a->timer != id || !a->dlg) return;
    if (a->next < a->nrows) {
        struct apps_row *r = &a->rows[a->next];
        apps_judge(a, r);
        if (r->verdict == APPS_V_WARN) apps_log_warning(r);
        apps_update_row(a, a->next);
        if (apps_selected(a, a->dlg) == a->next) apps_show_detail(a, a->dlg, a->next);
        a->next++;
    }
    apps_banner(a, a->dlg);
    if (a->next >= a->nrows) apps_run_stop(a);
}

static void apps_run_start(struct apps_data *a, dlgparam *dlg)
{
    apps_run_stop(a);
    apps_scan(a);
    a->next = 0;
    apps_fill(a, dlg);
    apps_show_detail(a, dlg, -1);
    apps_banner(a, dlg);
    if (a->nrows)
        a->timer = SetTimer(NULL, 0, 10, apps_timer_proc);
}

static void apps_box_closing(void)
{
    struct apps_data *a = kitty_apps_active;
    void (*prev)(void) = a ? a->prev_closing : NULL;
    if (a) {
        apps_run_stop(a);
        apps_free_rows(a);
        a->dlg = NULL;
    }
    kitty_apps_active = NULL;
    if (prev) prev();
}

/* ---- the panel ---------------------------------------------------------------- */

static void kitty_apps_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    struct apps_data *a = (struct apps_data *)ctrl->context.p;
    int which = ctrl->context2.i;          /* 0 list, 1 detail, 2 copy, 3 verify again */
    (void)data;
    if (!a) return;
    a->dlg = dlg;
    switch (which) {
      case 0:
        if (event == EVENT_REFRESH) {
            if (!a->rows) apps_run_start(a, dlg);
            else { apps_fill(a, dlg); apps_banner(a, dlg); }
        } else if (event == EVENT_SELCHANGE) {
            HWND h = kitty_cfg_ctrl_hwnd(ctrl);
            if (h && SendMessage(h, LB_GETSEL, 0, 0) > 0) {
                /* the header row: not a file; it shows the strangers */
                SendMessage(h, LB_SETSEL, FALSE, 0);
                apps_show_detail(a, dlg, -1);
                break;
            }
            apps_show_detail(a, dlg, apps_selected(a, dlg));
        }
        break;
      case 1:
        break;
      case 2:
        if (event == EVENT_ACTION) {
            strbuf *sb = strbuf_new();
            int i;
            put_fmt(sb, "%s\r\n", KT_APPS_COL_HEAD);
            for (i = 0; i < a->nrows; i++) {
                char *row = apps_row_text(a, &a->rows[i]);
                put_fmt(sb, "%s\r\n", row);
                sfree(row);
            }
            for (i = 0; i < a->nstrangers; i++)
                put_fmt(sb, "%s\t%s\r\n", a->strangers[i], KT_APPS_STRANGER);
            SetTextToClipboard(sb->s);
            strbuf_free(sb);
            dlg_label_change(a->banner, dlg, KT_APPS_COPIED);
        }
        break;
      case 3:
        if (event == EVENT_ACTION) apps_run_start(a, dlg);
        break;
    }
}

void scb_panel_applications(struct controlbox *b)
{
    static const char *const path = KCFG_PATH_APPLICATIONS;
    struct apps_data *a = (struct apps_data *)ctrl_alloc(b, sizeof(*a));
    struct controlset *s;
    dlgcontrol *c;

    /* A previous box's run must not tick into freed panel state. */
    if (kitty_apps_active) apps_run_stop(kitty_apps_active);
    memset(a, 0, sizeof(*a));
    kitty_apps_active = a;
    /* One closing hook for the box: keep whoever set it before us in the chain. */
    a->prev_closing = (kitty_cfg_box_closing_hook == apps_box_closing) ? NULL : kitty_cfg_box_closing_hook;
    kitty_cfg_box_closing_hook = apps_box_closing;

    ctrl_settitle(b, path, KT_APPS_TITLE);
    s = ctrl_getset(b, path, "apps", NULL);
    ctrl_text(s, KT_APPS_INTRO, HELPCTX(kitty_applications));
    a->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT, HELPCTX(kitty_applications), kitty_apps_handler, P(a));
    a->listbox->context2 = I(0);
    a->listbox->listbox.height = 4;     /* the floor; the fill hook grows it */
    a->listbox->listbox.multisel = 2;   /* extended: the arrow keys select */
    a->listbox->listbox.headerrow = true;
    a->listbox->listbox.rowink = apps_row_ink;
    a->listbox->listbox.ncols = 5;
    a->listbox->listbox.percentages = snewn(5, int);
    a->listbox->listbox.percentages[0] = 26;   /* File */
    a->listbox->listbox.percentages[1] = 14;   /* Version */
    a->listbox->listbox.percentages[2] = 22;   /* Signature */
    a->listbox->listbox.percentages[3] = 20;   /* Stamp */
    a->listbox->listbox.percentages[4] = 18;   /* Verdict */
    a->detail = ctrl_editbox_multiline(s, NULL, NO_SHORTCUT, 5, true, HELPCTX(kitty_applications),
                                       kitty_apps_handler, P(a), P(NULL));
    a->detail->context2 = I(1);
    a->banner = ctrl_text(s, " ", HELPCTX(kitty_applications));
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_pushbutton(s, KT_APPS_COPY, NO_SHORTCUT, HELPCTX(kitty_applications), kitty_apps_handler, P(a));
    c->context2 = I(2); c->column = 0; a->copy = c;
    c = ctrl_pushbutton(s, KT_APPS_VERIFY, NO_SHORTCUT, HELPCTX(kitty_applications), kitty_apps_handler, P(a));
    c->context2 = I(3); c->column = 1; a->verify = c;
    ctrl_columns(s, 1, 100);
    a->note = ctrl_text(s, KT_APPS_NOTE, HELPCTX(kitty_applications));
}
