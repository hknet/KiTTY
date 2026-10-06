/*
 * kitty_tags.c - session tags (hknet/KiTTY#60); see kitty_tags.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"
#include "kitty_text.h"          /* KT_STARTUP_TAGS_* */

#include "kitty_tags.h"
#include "kitty_sessionpath.h"   /* kitty_session_names */
#include "kitty_storage.h"       /* kitty_store_mark_dirty */

bool kitty_tag_name_ok(const char *tag)
{
    size_t n;
    if (!tag)
        return false;
    n = strlen(tag);
    return n > 0 && n <= KITTY_TAG_MAXLEN && !strchr(tag, ',') &&
        !isspace((unsigned char)tag[0]) && !isspace((unsigned char)tag[n - 1]);
}

char **kitty_tags_split(const char *tags, int *n)
{
    char **v = NULL;
    const char *p = tags ? tags : "";
    *n = 0;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *a = p, *b = p + len;
        int i, dup = 0;
        while (a < b && isspace((unsigned char)*a))
            a++;
        while (b > a && isspace((unsigned char)b[-1]))
            b--;
        if (b > a) {
            char *t = dupprintf("%.*s", (int)(b - a), a);
            for (i = 0; i < *n; i++)
                if (!stricmp(v[i], t))
                    dup = 1;
            if (dup) {
                sfree(t);
            } else {
                v = sresize(v, *n + 1, char *);
                v[(*n)++] = t;
            }
        }
        p += len;
        if (*p == ',')
            p++;
    }
    return v;
}

void kitty_tags_free(char **v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        sfree(v[i]);
    sfree(v);
}

char *kitty_tags_join(char **v, int n)
{
    strbuf *sb = strbuf_new();
    int i;
    for (i = 0; i < n; i++) {
        if (i)
            put_byte(sb, ',');
        put_dataz(sb, v[i]);
    }
    return strbuf_to_str(sb);
}

bool kitty_tags_has(const char *tags, const char *tag)
{
    int n, i;
    bool has = false;
    char **v = kitty_tags_split(tags, &n);
    for (i = 0; i < n; i++)
        if (!stricmp(v[i], tag))
            has = true;
    kitty_tags_free(v, n);
    return has;
}

char *kitty_tags_add(const char *tags, const char *tag)
{
    int n;
    char **v = kitty_tags_split(tags, &n), *out;
    if (!kitty_tags_has(tags, tag)) {
        v = sresize(v, n + 1, char *);
        v[n++] = dupstr(tag);
    }
    out = kitty_tags_join(v, n);
    kitty_tags_free(v, n);
    return out;
}

char *kitty_tags_remove(const char *tags, const char *tag)
{
    int n, i, k = 0;
    char **v = kitty_tags_split(tags, &n), *out;
    for (i = 0; i < n; i++) {
        if (!stricmp(v[i], tag))
            sfree(v[i]);
        else
            v[k++] = v[i];
    }
    out = kitty_tags_join(v, k);
    kitty_tags_free(v, k);
    return out;
}

/* Eight colours each way: dark enough to read on a light list, light enough
 * on a dark one, and far enough apart to tell side by side. */
COLORREF kitty_tag_colour(const char *tag, bool dark)
{
    static const COLORREF light_ink[8] = {
        RGB(0, 90, 170),  RGB(0, 120, 60),  RGB(160, 70, 0),  RGB(140, 0, 120),
        RGB(0, 110, 120), RGB(120, 90, 0),  RGB(170, 0, 40),  RGB(80, 60, 160) };
    static const COLORREF dark_ink[8] = {
        RGB(110, 180, 255), RGB(110, 210, 140), RGB(255, 170, 90),  RGB(230, 130, 220),
        RGB(90, 210, 220),  RGB(230, 200, 90),  RGB(255, 120, 140), RGB(170, 150, 255) };
    unsigned h = 2166136261u;
    const char *p;
    for (p = tag ? tag : ""; *p; p++)
        h = (h ^ (unsigned char)tolower((unsigned char)*p)) * 16777619u;
    return dark ? dark_ink[h % 8] : light_ink[h % 8];
}

char *kitty_session_tags(const char *session)
{
    settings_r *r = open_settings_r(session);
    char *v = NULL;
    if (r) {
        v = read_setting_s(r, "Tags");
        close_settings_r(r);
    }
    return v ? v : dupstr("");
}

bool kitty_session_set_tags(const char *session, const char *tags)
{
    char *errmsg = NULL;
    settings_w *w = open_settings_w(session, &errmsg);
    if (!w) {
        sfree(errmsg);
        return false;
    }
    write_setting_s(w, "Tags", tags ? tags : "");
    close_settings_w(w);
    kitty_store_mark_dirty();
    return true;
}

static int kitty_tag_cmp(const void *a, const void *b)
{
    return stricmp(*(char *const *)a, *(char *const *)b);
}

char **kitty_all_tags(int *n)
{
    int ns, i, j;
    char **names = kitty_session_names(&ns), **all = NULL;
    *n = 0;
    for (i = 0; i < ns; i++) {
        char *t = kitty_session_tags(names[i]);
        int nt;
        char **v = kitty_tags_split(t, &nt);
        for (j = 0; j < nt; j++) {
            int k, have = 0;
            for (k = 0; k < *n; k++)
                if (!stricmp(all[k], v[j]))
                    have = 1;
            if (!have) {
                all = sresize(all, *n + 1, char *);
                all[(*n)++] = dupstr(v[j]);
            }
        }
        kitty_tags_free(v, nt);
        sfree(t);
    }
    kitty_session_names_free(names, ns);
    if (*n > 1)
        qsort(all, *n, sizeof(char *), kitty_tag_cmp);
    return all;
}

/* ---- Session > Startup, "Session Tags" ----
 * The list shows the Conf's tags, coloured; Remove (or Del) drops the
 * selected ones. "New tag:" offers every tag in use - read from the store
 * only when its list first opens, since that reads every session - and Add
 * (or Enter in the field) puts the typed one on. Everything changes the
 * Conf only; the session's Save writes it, as for any other setting. */
static struct {
    dlgcontrol *list, *combo;
    dlgparam *dlg;
    Conf *conf;
    char **cur, **all;   /* the list's rows; every tag in use (NULL = unread) */
    int ncur, nall;
} kt;

static bool kt_row_ink(dlgcontrol *ctrl, int id, bool dark, unsigned long *rgb)
{
    (void)ctrl;
    if (id < 0 || id >= kt.ncur)
        return false;
    *rgb = kitty_tag_colour(kt.cur[id], dark);
    return true;
}

static void kt_set(Conf *conf, const char *tags)
{
    conf_set_str(conf, CONF_tags, tags);
}

static void kt_remove_selected(dlgparam *dlg, Conf *conf)
{
    char *tags = dupstr(conf_get_str(conf, CONF_tags));
    int i, removed = 0;
    for (i = 0; i < kt.ncur; i++)
        if (dlg_listbox_issel(kt.list, dlg, i)) {
            char *t = kitty_tags_remove(tags, kt.cur[i]);
            sfree(tags);
            tags = t;
            removed++;
        }
    if (removed) {
        kt_set(conf, tags);
        dlg_refresh(kt.list, dlg);
    } else
        dlg_beep(dlg);
    sfree(tags);
}

static void kt_add_typed(dlgparam *dlg, Conf *conf)
{
    char *typed = dlg_editbox_get(kt.combo, dlg), *a = typed, *b, *tags;
    while (*a == ' ' || *a == '\t')
        a++;
    b = a + strlen(a);
    while (b > a && (b[-1] == ' ' || b[-1] == '\t'))
        *--b = '\0';
    if (!kitty_tag_name_ok(a)) {
        dlg_beep(dlg);
        sfree(typed);
        return;
    }
    tags = kitty_tags_add(conf_get_str(conf, CONF_tags), a);
    kt_set(conf, tags);
    sfree(tags);
    if (kt.all) {
        int i, have = 0;
        for (i = 0; i < kt.nall; i++)
            if (!stricmp(kt.all[i], a))
                have = 1;
        if (!have) {
            kt.all = sresize(kt.all, kt.nall + 1, char *);
            kt.all[kt.nall++] = dupstr(a);
            qsort(kt.all, kt.nall, sizeof(char *), kitty_tag_cmp);
        }
    }
    sfree(typed);
    dlg_editbox_set(kt.combo, dlg, "");
    dlg_refresh(kt.list, dlg);
}

static bool kt_row_key(dlgcontrol *ctrl, int vk)
{
    (void)ctrl;
    if (vk != VK_DELETE || !kt.dlg || !kt.conf ||
        GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 ||
        GetKeyState(VK_SHIFT) < 0)
        return false;
    kt_remove_selected(kt.dlg, kt.conf);
    return true;
}

/* The combo's list: every tag in use the session does not carry yet. */
static void kt_dropdown(dlgcontrol *ctrl, dlgparam *dlg)
{
    char *typed = dlg_editbox_get(ctrl, dlg);
    const char *tags = kt.conf ? conf_get_str(kt.conf, CONF_tags) : "";
    int i;
    if (!kt.all)
        kt.all = kitty_all_tags(&kt.nall);
    dlg_update_start(ctrl, dlg);
    dlg_listbox_clear(ctrl, dlg);
    for (i = 0; i < kt.nall; i++)
        if (!kitty_tags_has(tags, kt.all[i]))
            dlg_listbox_add(ctrl, dlg, kt.all[i]);
    dlg_update_done(ctrl, dlg);
    dlg_editbox_set(ctrl, dlg, typed);
    sfree(typed);
}

/* context2: 0 the list, 1 Remove, 2 the combo, 3 Add. */
static void kt_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    Conf *conf = (Conf *)data;
    int i;
    kt.dlg = dlg;
    kt.conf = conf;
    switch (ctrl->context2.i) {
      case 0:
        if (event == EVENT_REFRESH) {
            kitty_tags_free(kt.cur, kt.ncur);
            kt.cur = kitty_tags_split(conf_get_str(conf, CONF_tags), &kt.ncur);
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < kt.ncur; i++)
                dlg_listbox_addwithid(ctrl, dlg, kt.cur[i], i);
            dlg_update_done(ctrl, dlg);
        }
        break;
      case 1:
        if (event == EVENT_ACTION)
            kt_remove_selected(dlg, conf);
        break;
      case 2:
        /* Nothing on REFRESH: the box's own refresh (it comes with every
         * focus loss) would take the typed text away before Add reads it. */
        break;
      case 3:
        if (event == EVENT_ACTION)
            kt_add_typed(dlg, conf);
        break;
    }
}

void kitty_tags_controls(struct controlset *s)
{
    dlgcontrol *c;
    kitty_tags_free(kt.cur, kt.ncur);
    kitty_tags_free(kt.all, kt.nall);
    memset(&kt, 0, sizeof(kt));

    /* The list's label on a line of its own, so the list and Remove start
     * on one row (the panel's row pass places a button beside a tall list,
     * it does not centre it). */
    ctrl_text(s, KT_STARTUP_TAGS_LIST, HELPCTX(kitty_session_tags));
    ctrl_columns(s, 2, 75, 25);
    c = ctrl_listbox(s, NULL, NO_SHORTCUT,
                     HELPCTX(kitty_session_tags), kt_handler, P(NULL));
    c->context2 = I(0);
    c->column = 0;
    c->listbox.height = 4;
    c->listbox.multisel = 2;
    c->listbox.rowink = kt_row_ink;
    c->listbox.rowkey = kt_row_key;
    kt.list = c;
    c = ctrl_pushbutton(s, KT_STARTUP_TAGS_REMOVE, NO_SHORTCUT,
                        HELPCTX(kitty_session_tags), kt_handler, P(NULL));
    c->context2 = I(1);
    c->column = 1;
    /* a fresh pair of columns: Add beside the field, not under Remove */
    ctrl_columns(s, 1, 100);
    ctrl_columns(s, 2, 75, 25);
    /* label beside the field, Add centred on the field (align_next_to) */
    c = ctrl_combobox(s, KT_STARTUP_TAGS_NEW, NO_SHORTCUT, 70,
                      HELPCTX(kitty_session_tags), kt_handler, P(NULL), I(2));
    c->column = 0;
    c->editbox.dropdown = kt_dropdown;
    kt.combo = c;
    c = ctrl_pushbutton(s, KT_STARTUP_TAGS_ADD, NO_SHORTCUT,
                        HELPCTX(kitty_session_tags), kt_handler, P(NULL));
    c->context2 = I(3);
    c->column = 1;
    c->align_next_to = kt.combo;
    ctrl_columns(s, 1, 100);
    ctrl_text(s, KT_STARTUP_TAGS_NOTE, HELPCTX(kitty_session_tags));
}

bool kitty_tags_enter(dlgcontrol *okbutton, dlgparam *dlg, Conf *conf)
{
    /* Enter typed in the field, not a click on the button itself. */
    if (!kt.combo || dlg_is_focused(okbutton, dlg) ||
        dlg_last_focused(okbutton, dlg) != kt.combo)
        return false;
    kt_add_typed(dlg, conf);
    return true;
}
