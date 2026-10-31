/*
 * kitty_sessorder.c - the manual order of folders and sessions. The model
 * and the two stores are described in kitty_sessorder.h.
 *
 * Linked beside kitty_sessionpath.c everywhere (the settings library, and
 * kageant without it): nothing here needs the settings library. kitty.exe's
 * description of the store in use is ksp_order_store_current() in
 * kitty_storage.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty_sessionpath.h"
#include "kitty_sessorder.h"
#include "kitty_atomicfile.h"
#include "kitty_inifile.h"      /* kitty_store_readonly: readonly=yes */

/* ======================================================================
 * Lists
 * ====================================================================== */

void ksp_order_list_init(struct ksp_order_list *l)
{
    memset(l, 0, sizeof(*l));
}

void ksp_order_list_free(struct ksp_order_list *l)
{
    int i;
    for (i = 0; i < l->n; i++) {
        sfree(l->e[i].name);
        sfree(l->e[i].anchor);
    }
    sfree(l->e);
    memset(l, 0, sizeof(*l));
}

void ksp_order_list_add(struct ksp_order_list *l, char kind, const char *name,
                        const char *anchor)
{
    sgrowarray(l->e, l->cap, l->n);
    l->e[l->n].kind = kind;
    l->e[l->n].name = dupstr(name);
    l->e[l->n].anchor = anchor ? dupstr(anchor) : NULL;
    l->n++;
}

const struct ksp_order_entry *ksp_order_find(const struct ksp_order_list *l,
                                             char kind, const char *name)
{
    int i;
    if (!l || !name)
        return NULL;
    for (i = 0; i < l->n; i++)
        if (l->e[i].kind == kind && l->e[i].name && !stricmp(l->e[i].name, name))
            return &l->e[i];
    return NULL;
}

static int kso_anchor_same(const char *a, const char *b)
{
    if (!a || !b)
        return !a && !b;
    return !stricmp(a, b);
}

void ksp_order_changes_free(struct ksp_order_changes *ch)
{
    int i;
    for (i = 0; i < ch->n; i++) {
        sfree(ch->c[i].name);
        sfree(ch->c[i].anchor);
    }
    sfree(ch->c);
    memset(ch, 0, sizeof(*ch));
}

static void kso_change_add(struct ksp_order_changes *ch, char kind,
                           const char *name, const char *anchor, int remove,
                           int moved)
{
    sgrowarray(ch->c, ch->cap, ch->n);
    ch->c[ch->n].kind = kind;
    ch->c[ch->n].name = dupstr(name);
    ch->c[ch->n].anchor = anchor ? dupstr(anchor) : NULL;
    ch->c[ch->n].remove = remove;
    ch->c[ch->n].moved = moved;
    ch->n++;
}

/* ======================================================================
 * The items of a level as units: one unit per shown name (without case),
 * numbered in the automatic order of their first item.
 * ====================================================================== */

struct kso_key {
    const char *name;
    int idx;
};
static int kso_key_cmp(const void *av, const void *bv)
{
    const struct kso_key *a = av, *b = bv;
    int c = stricmp(a->name, b->name);
    return c ? c : (a->idx > b->idx) - (a->idx < b->idx);
}

struct kso_units {
    int n, nu;
    int *unit_of;          /* item -> unit */
    int *ihead, *inext;    /* a unit's items, in the automatic order */
    struct kso_key *uk;    /* one per unit, sorted by name: the lookup */
};

static void kso_units_build(struct kso_units *u, char *const *names, int n)
{
    struct kso_key *k = snewn(n > 0 ? n : 1, struct kso_key);
    int *rep = snewn(n > 0 ? n : 1, int);
    int *uid = snewn(n > 0 ? n : 1, int);
    int *itail;
    int i, j;

    memset(u, 0, sizeof(*u));
    u->n = n;
    for (i = 0; i < n; i++) {
        k[i].name = names[i] ? names[i] : "";
        k[i].idx = i;
    }
    if (n > 1)
        qsort(k, n, sizeof(*k), kso_key_cmp);
    /* every item of a group of equal names takes the group's first item */
    for (i = 0; i < n; ) {
        int first = k[i].idx;
        for (j = i; j < n && !stricmp(k[j].name, k[i].name); j++)
            if (k[j].idx < first)
                first = k[j].idx;
        for (; i < j; i++)
            rep[k[i].idx] = first;
    }
    u->nu = 0;
    for (i = 0; i < n; i++)
        uid[i] = (rep[i] == i) ? u->nu++ : -1;
    u->unit_of = snewn(n > 0 ? n : 1, int);
    for (i = 0; i < n; i++)
        u->unit_of[i] = uid[rep[i]];
    u->uk = snewn(u->nu > 0 ? u->nu : 1, struct kso_key);
    for (i = 0, j = 0; i < n; i++)
        if (rep[k[i].idx] == k[i].idx) {
            u->uk[j].name = k[i].name;
            u->uk[j].idx = uid[k[i].idx];
            j++;
        }
    u->ihead = snewn(u->nu > 0 ? u->nu : 1, int);
    itail = snewn(u->nu > 0 ? u->nu : 1, int);
    u->inext = snewn(n > 0 ? n : 1, int);
    for (i = 0; i < u->nu; i++)
        u->ihead[i] = itail[i] = -1;
    for (i = 0; i < n; i++) {
        int un = u->unit_of[i];
        u->inext[i] = -1;
        if (itail[un] < 0)
            u->ihead[un] = i;
        else
            u->inext[itail[un]] = i;
        itail[un] = i;
    }
    sfree(itail);
    sfree(uid);
    sfree(rep);
    sfree(k);
}

static void kso_units_free(struct kso_units *u)
{
    sfree(u->unit_of);
    sfree(u->ihead);
    sfree(u->inext);
    sfree(u->uk);
    memset(u, 0, sizeof(*u));
}

/* The unit of a name, or -1. */
static int kso_units_find(const struct kso_units *u, const char *name)
{
    int lo = 0, hi = u->nu - 1;
    if (!name)
        return -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = stricmp(name, u->uk[mid].name);
        if (!c)
            return u->uk[mid].idx;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return -1;
}

/* ======================================================================
 * The resolver
 * ====================================================================== */

#define KSO_FIRST (-1)
#define KSO_NONE  (-2)

struct kso_seq {
    int unit, seq;
};
static int kso_seq_cmp(const void *av, const void *bv)
{
    const struct kso_seq *a = av, *b = bv;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

/* Units in the order shown (out[], nu of them) and which are placed. */
static void kso_resolve(const struct ksp_order_list *l, char kind,
                        const struct kso_units *u, int *out,
                        unsigned char *placed)
{
    int nu = u->nu, i, k, no = 0, sp, c;
    int *anc = snewn(nu + 1, int), *seq = snewn(nu + 1, int);
    int *color = snewn(nu + 1, int), *path = snewn(nu + 1, int);
    int *head = snewn(nu + 1, int), *tail = snewn(nu + 1, int);
    int *next = snewn(nu + 1, int), *stack = snewn(nu + 1, int);
    unsigned char *done = snewn(nu + 1, unsigned char);
    struct kso_seq *ord = snewn(nu + 1, struct kso_seq);
    int nord = 0;

    for (i = 0; i < nu; i++) {
        anc[i] = KSO_NONE;
        seq[i] = -1;
        placed[i] = 0;
        color[i] = 0;
        done[i] = 0;
    }
    /* the first entry of a name counts; an anchor that is not an item of
     * the level (or the item itself) leaves the item where it was */
    for (k = 0; l && k < l->n; k++) {
        const struct ksp_order_entry *e = &l->e[k];
        int un;
        if (e->kind != kind || !e->name)
            continue;
        un = kso_units_find(u, e->name);
        if (un < 0 || seq[un] >= 0)
            continue;
        seq[un] = k;
        if (!e->anchor) {
            anc[un] = KSO_FIRST;
        } else {
            int a = kso_units_find(u, e->anchor);
            anc[un] = (a < 0 || a == un) ? KSO_NONE : a;
        }
        placed[un] = anc[un] != KSO_NONE;
    }
    /* a cycle cannot be resolved: its members keep their automatic place */
    for (i = 0; i < nu; i++) {
        int np = 0, p = i, j;
        if (!placed[i] || color[i])
            continue;
        while (p >= 0 && placed[p] && color[p] == 0) {
            color[p] = 1;
            path[np++] = p;
            p = anc[p];
        }
        if (p >= 0 && placed[p] && color[p] == 1) {
            for (j = np - 1; j >= 0; j--) {
                placed[path[j]] = 0;
                if (path[j] == p)
                    break;
            }
        }
        for (j = 0; j < np; j++)
            color[path[j]] = 2;
    }
    /* each anchor's followers, in the order of their entries */
    for (i = 0; i <= nu; i++)
        head[i] = tail[i] = -1;
    for (i = 0; i < nu; i++)
        if (placed[i]) {
            ord[nord].unit = i;
            ord[nord].seq = seq[i];
            nord++;
        }
    if (nord > 1)
        qsort(ord, nord, sizeof(*ord), kso_seq_cmp);
    for (k = 0; k < nord; k++) {
        int un = ord[k].unit, par = anc[un] == KSO_FIRST ? nu : anc[un];
        next[un] = -1;
        if (tail[par] < 0)
            head[par] = un;
        else
            next[tail[par]] = un;
        tail[par] = un;
    }
    /* "first" and its chains, then every automatic item followed by its
     * chains: a depth-first walk without recursion */
    for (k = -1; k < nu; k++) {
        int par;
        if (k >= 0) {
            if (placed[k])
                continue;
            out[no++] = k;
            done[k] = 1;
            par = k;
        } else {
            par = nu;
        }
        sp = 0;
        c = head[par];
        for (;;) {
            if (c >= 0) {
                if (!done[c]) {
                    out[no++] = c;
                    done[c] = 1;
                }
                stack[sp++] = next[c];
                c = head[c];
            } else if (sp > 0) {
                c = stack[--sp];
            } else {
                break;
            }
        }
    }
    /* cannot happen (every chain ends at "first" or an automatic item);
     * kept so that a mistake loses no row */
    for (i = 0; i < nu && no < nu; i++)
        if (!done[i]) {
            out[no++] = i;
            placed[i] = 0;
        }

    sfree(ord);
    sfree(done);
    sfree(stack);
    sfree(next);
    sfree(tail);
    sfree(head);
    sfree(path);
    sfree(color);
    sfree(seq);
    sfree(anc);
}

void ksp_order_apply(const struct ksp_order_list *l, char kind,
                     char *const *names, int n, int *perm,
                     unsigned char *pinned)
{
    struct kso_units u;
    int *out, k, i, np = 0;
    unsigned char *placed;

    if (n <= 0)
        return;
    kso_units_build(&u, names, n);
    out = snewn(u.nu + 1, int);
    placed = snewn(u.nu + 1, unsigned char);
    kso_resolve(l, kind, &u, out, placed);
    for (k = 0; k < u.nu; k++)
        for (i = u.ihead[out[k]]; i >= 0; i = u.inext[i]) {
            perm[np++] = i;
            if (pinned)
                pinned[i] = placed[out[k]];
        }
    sfree(placed);
    sfree(out);
    kso_units_free(&u);
}

/* ======================================================================
 * The planner
 * ====================================================================== */

int ksp_order_plan(const struct ksp_order_list *old, char kind,
                   char *const *autonames, int nauto,
                   char *const *desired, int ndesired,
                   char *const *moved, int nmoved,
                   char *const *gone, int ngone,
                   struct ksp_order_changes *changes)
{
    struct kso_units du;
    int nd, i, j, k, iter, *perm;
    const char **dn;                /* the desired order, one per name */
    unsigned char *has, *ohas, *ismoved, *fixed;
    const char **anc, **oanc;
    const char *A = NULL;           /* the row the block was dropped below */
    int a_is_first = 0;

    memset(changes, 0, sizeof(*changes));
    /* one per name, in the order wanted */
    dn = snewn(ndesired > 0 ? ndesired : 1, const char *);
    nd = 0;
    for (i = 0; i < ndesired; i++) {
        for (j = 0; j < nd && stricmp(dn[j], desired[i]); j++)
            ;
        if (j == nd)
            dn[nd++] = desired[i];
    }
    kso_units_build(&du, (char *const *)dn, nd);   /* unit k = dn[k] */
    has = snewn(nd + 1, unsigned char);
    ohas = snewn(nd + 1, unsigned char);
    ismoved = snewn(nd + 1, unsigned char);
    fixed = snewn(nd + 1, unsigned char);
    anc = snewn(nd + 1, const char *);
    oanc = snewn(nd + 1, const char *);
    for (i = 0; i < nd; i++) {
        has[i] = ohas[i] = ismoved[i] = fixed[i] = 0;
        anc[i] = oanc[i] = NULL;
    }
    for (k = 0; old && k < old->n; k++) {
        const struct ksp_order_entry *e = &old->e[k];
        int un;
        if (e->kind != kind)
            continue;
        un = kso_units_find(&du, e->name);
        if (un < 0 || has[un])
            continue;
        has[un] = ohas[un] = 1;
        anc[un] = oanc[un] = e->anchor;
    }
#define KSO_PRED(un) ((un) > 0 ? dn[(un) - 1] : NULL)
    if (nmoved > 0) {
        int u0 = kso_units_find(&du, moved[0]);
        if (u0 >= 0) {
            A = KSO_PRED(u0);
            a_is_first = (A == NULL);
        }
    }
    for (i = 0; i < nmoved; i++) {
        int un = kso_units_find(&du, moved[i]);
        if (un < 0)
            continue;
        has[un] = 1;
        anc[un] = KSO_PRED(un);
        ismoved[un] = 1;
    }
    /* the items anchored to a moved or gone item, or below the drop: they
     * stay where they were shown */
    for (i = 0; i < nd; i++) {
        int hit = 0;
        if (!has[i] || ismoved[i])
            continue;
        if (anc[i]) {
            for (j = 0; j < nmoved && !hit; j++)
                hit = !stricmp(anc[i], moved[j]);
            for (j = 0; j < ngone && !hit; j++)
                hit = !stricmp(anc[i], gone[j]);
            if (!hit && nmoved > 0 && A)
                hit = !stricmp(anc[i], A);
        } else {
            hit = nmoved > 0 && a_is_first;
        }
        if (hit)
            anc[i] = KSO_PRED(i);
    }
    /* whatever still resolves elsewhere is placed explicitly, item by item
     * from the top, until the order is the one wanted */
    perm = snewn(nauto > 0 ? nauto : 1, int);
    for (iter = 0; iter <= nd; iter++) {
        struct ksp_order_list l;
        int pos = 0, bad = -1;
        const char *last = NULL;
        ksp_order_list_init(&l);
        for (i = 0; i < nd; i++)
            if (has[i])
                ksp_order_list_add(&l, kind, dn[i], anc[i]);
        ksp_order_apply(&l, kind, autonames, nauto, perm, NULL);
        ksp_order_list_free(&l);
        for (k = 0; k < nauto && bad < 0; k++) {
            const char *nm = autonames[perm[k]];
            if (last && !stricmp(last, nm))
                continue;               /* the same name again: one entry */
            last = nm;
            if (pos >= nd || stricmp(nm, dn[pos]))
                bad = pos < nd ? pos : nd - 1;
            pos++;
        }
        if (bad < 0)
            break;
        if (bad < 0 || fixed[bad])
            break;                      /* cannot get closer: leave it */
        fixed[bad] = 1;
        has[bad] = 1;
        anc[bad] = KSO_PRED(bad);
    }
    sfree(perm);
#undef KSO_PRED
    for (i = 0; i < ngone; i++)
        if (ksp_order_find(old, kind, gone[i]))
            kso_change_add(changes, kind, gone[i], NULL, 1, 0);
    for (i = 0; i < nd; i++) {
        if (!has[i])
            continue;
        if (ohas[i] && kso_anchor_same(anc[i], oanc[i]) && !ismoved[i])
            continue;
        if (ohas[i] && kso_anchor_same(anc[i], oanc[i]) && ismoved[i])
            continue;                   /* dropped where it already was */
        kso_change_add(changes, kind, dn[i], anc[i], 0,
                       ismoved[i] || !ohas[i]);
    }
    sfree(oanc);
    sfree(anc);
    sfree(fixed);
    sfree(ismoved);
    sfree(ohas);
    sfree(has);
    kso_units_free(&du);
    sfree(dn);
    return changes->n;
}

/* ======================================================================
 * The document: a file as lines, or the registry's values
 * ====================================================================== */

struct kso_line {
    char *text;             /* without the line end */
    const char *eol;        /* "\r\n", "\n" or "" (the last line) */
    int item;               /* an item line */
    char kind;
    char *name, *anchor;    /* anchor NULL: the two-field form */
    int dead;
};

struct ksp_order_doc {
    int is_reg;
    int bom;
    struct kso_line *l;
    int n;
    size_t cap;
    char *orig;             /* the bytes read (file) */
    size_t origlen;
    struct ksp_order_list vals, ovals;  /* registry: now and as read */
};

static void kso_parse_line(struct kso_line *ln)
{
    const char *t = ln->text, *nm, *tab;
    size_t nl;
    sfree(ln->name);
    sfree(ln->anchor);
    ln->name = ln->anchor = NULL;
    ln->item = 0;
    ln->kind = 0;
    if (!t[0] || t[0] == '#')
        return;
    if ((t[0] != KSP_ORDER_SESSION && t[0] != KSP_ORDER_FOLDER) || t[1] != '\t')
        return;                         /* not readable: skipped */
    nm = t + 2;
    tab = strchr(nm, '\t');
    nl = tab ? (size_t)(tab - nm) : strlen(nm);
    if (!nl)
        return;
    ln->name = dupprintf("%.*s", (int)nl, nm);
    if (tab) {
        const char *an = tab + 1, *t2 = strchr(an, '\t');
        size_t al = t2 ? (size_t)(t2 - an) : strlen(an);
        if (al)
            ln->anchor = dupprintf("%.*s", (int)al, an);
    }
    ln->kind = t[0];
    ln->item = 1;
}

static const char *kso_doc_eol(const struct ksp_order_doc *d)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (d->l[i].eol[0])
            return d->l[i].eol;
    return "\r\n";
}

/* Insert a line at `at` (d->n = at the end), its text built from the
 * fields. */
static void kso_insert(struct ksp_order_doc *d, int at, char kind,
                       const char *name, const char *anchor)
{
    struct kso_line ln;
    const char *eol = kso_doc_eol(d);
    int i;
    memset(&ln, 0, sizeof(ln));
    ln.text = anchor ? dupprintf("%c\t%s\t%s", kind, name, anchor)
                     : dupprintf("%c\t%s", kind, name);
    ln.eol = eol;
    kso_parse_line(&ln);
    if (at >= d->n) {
        /* the last line gets its end now that one follows */
        for (i = d->n - 1; i >= 0; i--)
            if (!d->l[i].dead) {
                if (!d->l[i].eol[0])
                    d->l[i].eol = eol;
                break;
            }
        at = d->n;
    }
    sgrowarray(d->l, d->cap, d->n);
    memmove(&d->l[at + 1], &d->l[at], (d->n - at) * sizeof(*d->l));
    d->l[at] = ln;
    d->n++;
}

static void kso_line_rebuild(struct kso_line *ln)
{
    char *t = ln->anchor ? dupprintf("%c\t%s\t%s", ln->kind, ln->name, ln->anchor)
                         : dupprintf("%c\t%s", ln->kind, ln->name);
    sfree(ln->text);
    ln->text = t;
}

struct ksp_order_doc *ksp_order_doc_text(const char *buf, size_t len)
{
    struct ksp_order_doc *d = snew(struct ksp_order_doc);
    const char *p = buf, *end = buf + len;
    memset(d, 0, sizeof(*d));
    ksp_order_list_init(&d->vals);
    ksp_order_list_init(&d->ovals);
    d->orig = snewn(len + 1, char);
    if (len)
        memcpy(d->orig, buf, len);
    d->orig[len] = '\0';
    d->origlen = len;
    if (len >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
        d->bom = 1;
        p += 3;
    }
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        const char *te = nl ? nl : end;
        struct kso_line ln;
        memset(&ln, 0, sizeof(ln));
        if (nl) {
            if (te > p && te[-1] == '\r') {
                te--;
                ln.eol = "\r\n";
            } else {
                ln.eol = "\n";
            }
        } else {
            ln.eol = "";
        }
        ln.text = snewn((te - p) + 1, char);
        memcpy(ln.text, p, te - p);
        ln.text[te - p] = '\0';
        kso_parse_line(&ln);
        sgrowarray(d->l, d->cap, d->n);
        d->l[d->n++] = ln;
        p = nl ? nl + 1 : end;
    }
    return d;
}

static int kso_entry_cmp(const void *av, const void *bv)
{
    const struct ksp_order_entry *a = av, *b = bv;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : +1;
    return ksp_name_cmp(a->name, b->name);
}

struct ksp_order_doc *ksp_order_doc_values(const struct ksp_order_list *values)
{
    struct ksp_order_doc *d = snew(struct ksp_order_doc);
    int i;
    memset(d, 0, sizeof(*d));
    d->is_reg = 1;
    ksp_order_list_init(&d->vals);
    ksp_order_list_init(&d->ovals);
    for (i = 0; values && i < values->n; i++) {
        ksp_order_list_add(&d->vals, values->e[i].kind, values->e[i].name,
                           values->e[i].anchor);
        ksp_order_list_add(&d->ovals, values->e[i].kind, values->e[i].name,
                           values->e[i].anchor);
    }
    return d;
}

void ksp_order_doc_free(struct ksp_order_doc *d)
{
    int i;
    if (!d)
        return;
    for (i = 0; i < d->n; i++) {
        sfree(d->l[i].text);
        sfree(d->l[i].name);
        sfree(d->l[i].anchor);
    }
    sfree(d->l);
    sfree(d->orig);
    ksp_order_list_free(&d->vals);
    ksp_order_list_free(&d->ovals);
    sfree(d);
}

void ksp_order_doc_entries(const struct ksp_order_doc *d,
                           struct ksp_order_list *out)
{
    const char *prev_s = NULL, *prev_f = NULL;
    int i;
    ksp_order_list_init(out);
    if (!d)
        return;
    if (d->is_reg) {
        for (i = 0; i < d->vals.n; i++)
            ksp_order_list_add(out, d->vals.e[i].kind, d->vals.e[i].name,
                               d->vals.e[i].anchor);
        if (out->n > 1)
            qsort(out->e, out->n, sizeof(*out->e), kso_entry_cmp);
        return;
    }
    for (i = 0; i < d->n; i++) {
        const struct kso_line *ln = &d->l[i];
        const char **prev;
        if (ln->dead || !ln->item)
            continue;
        prev = ln->kind == KSP_ORDER_SESSION ? &prev_s : &prev_f;
        ksp_order_list_add(out, ln->kind, ln->name,
                           ln->anchor ? ln->anchor : *prev);
        *prev = ln->name;
    }
}

/* The first live item line of `kind` (from `from` on), named `name` when
 * that is not NULL; -1 = none. */
static int kso_find_line(const struct ksp_order_doc *d, char kind,
                         const char *name, int from)
{
    int i;
    for (i = from; i < d->n; i++)
        if (!d->l[i].dead && d->l[i].item && d->l[i].kind == kind &&
            (!name || !stricmp(d->l[i].name, name)))
            return i;
    return -1;
}

static void kso_kill_lines(struct ksp_order_doc *d, char kind, const char *name)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (!d->l[i].dead && d->l[i].item && d->l[i].kind == kind &&
            !stricmp(d->l[i].name, name))
            d->l[i].dead = 1;
}

static void kso_reg_set(struct ksp_order_list *v, char kind, const char *name,
                        const char *anchor, int remove)
{
    int i;
    for (i = 0; i < v->n; ) {
        if (v->e[i].kind == kind && !stricmp(v->e[i].name, name)) {
            if (!remove) {
                sfree(v->e[i].anchor);
                v->e[i].anchor = anchor ? dupstr(anchor) : NULL;
                return;
            }
            sfree(v->e[i].name);
            sfree(v->e[i].anchor);
            memmove(&v->e[i], &v->e[i + 1], (v->n - i - 1) * sizeof(*v->e));
            v->n--;
            continue;
        }
        i++;
    }
    if (!remove)
        ksp_order_list_add(v, kind, name, anchor);
}

static int kso_known(char *const *level, int nlevel, const char *name)
{
    int i;
    if (!level)
        return 1;
    for (i = 0; i < nlevel; i++)
        if (!stricmp(level[i], name))
            return 1;
    return 0;
}

void ksp_order_doc_change(struct ksp_order_doc *d, char kind,
                          char *const *level, int nlevel,
                          const struct ksp_order_changes *ch)
{
    struct ksp_order_list want;
    int i, pass;

    if (!d || !ch || !ch->n)
        return;
    if (d->is_reg) {
        for (i = 0; i < ch->n; i++)
            if (ch->c[i].kind == kind)
                kso_reg_set(&d->vals, kind, ch->c[i].name, ch->c[i].anchor,
                            ch->c[i].remove);
        return;
    }

    /* the places wanted: what the file means now, with the changes */
    {
        struct ksp_order_list cur;
        ksp_order_doc_entries(d, &cur);
        ksp_order_list_init(&want);
        for (i = 0; i < cur.n; i++)
            if (cur.e[i].kind == kind &&
                !ksp_order_find(&want, kind, cur.e[i].name))
                ksp_order_list_add(&want, kind, cur.e[i].name, cur.e[i].anchor);
        ksp_order_list_free(&cur);
        for (i = 0; i < ch->n; i++)
            if (ch->c[i].kind == kind)
                kso_reg_set(&want, kind, ch->c[i].name, ch->c[i].anchor,
                            ch->c[i].remove);
        /* a text can put only one line first: several "first" places (an
         * import's beside the file's own) become a chain, which shows the
         * same order */
        {
            const char *prevfirst = NULL;
            for (i = 0; i < want.n; i++)
                if (!want.e[i].anchor) {
                    if (prevfirst)
                        want.e[i].anchor = dupstr(prevfirst);
                    prevfirst = want.e[i].name;
                }
        }
    }

    /* a forgotten place: its lines go */
    for (i = 0; i < ch->n; i++)
        if (ch->c[i].kind == kind && ch->c[i].remove)
            kso_kill_lines(d, kind, ch->c[i].name);

    /* a moved item: its line goes right after its anchor's line (the two-
     * field form), first of its kind, or - its anchor has no line - at the
     * end with the anchor written out */
    for (i = 0; i < ch->n; i++) {
        const struct ksp_order_change *c = &ch->c[i];
        int at;
        if (c->kind != kind || c->remove)
            continue;
        if (!c->moved && kso_find_line(d, kind, c->name, 0) >= 0)
            continue;                   /* stays put: its anchor, below */
        kso_kill_lines(d, kind, c->name);
        if (!c->anchor) {
            at = kso_find_line(d, kind, NULL, 0);
            kso_insert(d, at < 0 ? d->n : at, kind, c->name, NULL);
        } else {
            at = kso_find_line(d, kind, c->anchor, 0);
            if (at >= 0)
                kso_insert(d, at + 1, kind, c->name, NULL);
            else
                kso_insert(d, d->n, kind, c->name, c->anchor);
        }
    }

    /* every other line of a known name whose meaning is no longer the one
     * wanted: its anchor written out; "first" by moving it to the top */
    for (pass = 0; pass <= d->n + 1; pass++) {
        const char *prev = NULL;
        int changed = 0;
        for (i = 0; i < d->n && !changed; i++) {
            struct kso_line *ln = &d->l[i];
            const struct ksp_order_entry *w;
            const char *cur;
            int j, dup = 0;
            if (ln->dead || !ln->item || ln->kind != kind)
                continue;
            for (j = 0; j < i && !dup; j++)
                dup = !d->l[j].dead && d->l[j].item && d->l[j].kind == kind &&
                      !stricmp(d->l[j].name, ln->name);
            cur = ln->anchor ? ln->anchor : prev;
            if (!dup && kso_known(level, nlevel, ln->name) &&
                (w = ksp_order_find(&want, kind, ln->name)) != NULL &&
                !kso_anchor_same(cur, w->anchor)) {
                if (!w->anchor) {
                    char *nm = dupstr(ln->name);
                    int at = kso_find_line(d, kind, NULL, 0);
                    if (at < 0 || at > i)
                        at = i;
                    ln->dead = 1;
                    kso_insert(d, at, kind, nm, NULL);
                    sfree(nm);
                } else {
                    sfree(ln->anchor);
                    ln->anchor = dupstr(w->anchor);
                    kso_line_rebuild(ln);
                }
                changed = 1;
            }
            prev = d->l[i].name;
        }
        if (!changed)
            break;
    }
    ksp_order_list_free(&want);
}

void ksp_order_doc_rename(struct ksp_order_doc *d, char kind,
                          const char *from, const char *to)
{
    int i;
    if (!d || !from || !to)
        return;
    if (d->is_reg) {
        for (i = 0; i < d->vals.n; i++) {
            struct ksp_order_entry *e = &d->vals.e[i];
            if (e->kind != kind)
                continue;
            if (!stricmp(e->name, from)) {
                sfree(e->name);
                e->name = dupstr(to);
            }
            if (e->anchor && !stricmp(e->anchor, from)) {
                sfree(e->anchor);
                e->anchor = dupstr(to);
            }
        }
        return;
    }
    for (i = 0; i < d->n; i++) {
        struct kso_line *ln = &d->l[i];
        int touched = 0;
        if (ln->dead || !ln->item || ln->kind != kind)
            continue;
        if (!stricmp(ln->name, from) && strcmp(ln->name, to)) {
            sfree(ln->name);
            ln->name = dupstr(to);
            touched = 1;
        }
        if (ln->anchor && !stricmp(ln->anchor, from) && strcmp(ln->anchor, to)) {
            sfree(ln->anchor);
            ln->anchor = dupstr(to);
            touched = 1;
        }
        if (touched)
            kso_line_rebuild(ln);
    }
}

void ksp_order_doc_reset(struct ksp_order_doc *d)
{
    int i;
    if (!d)
        return;
    if (d->is_reg) {
        ksp_order_list_free(&d->vals);
        return;
    }
    for (i = 0; i < d->n; i++)
        if (d->l[i].item)
            d->l[i].dead = 1;
}

char *ksp_order_doc_bytes(const struct ksp_order_doc *d, size_t *len)
{
    strbuf *sb = strbuf_new();
    int i;
    if (d && !d->is_reg) {
        if (d->bom)
            put_data(sb, "\xEF\xBB\xBF", 3);
        for (i = 0; i < d->n; i++)
            if (!d->l[i].dead) {
                put_dataz(sb, d->l[i].text);
                put_dataz(sb, d->l[i].eol);
            }
    }
    *len = sb->len;
    return strbuf_to_str(sb);
}

int ksp_order_doc_empty(const struct ksp_order_doc *d)
{
    int i;
    if (!d)
        return 1;
    if (d->is_reg)
        return d->vals.n == 0;
    for (i = 0; i < d->n; i++) {
        const char *t = d->l[i].text;
        if (d->l[i].dead)
            continue;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t)
            return 0;                   /* an item, a comment or other text */
    }
    return 1;
}

int ksp_order_doc_changed(const struct ksp_order_doc *d)
{
    int i, ch;
    if (!d)
        return 0;
    if (d->is_reg) {
        if (d->vals.n != d->ovals.n)
            return 1;
        for (i = 0; i < d->vals.n; i++) {
            const struct ksp_order_entry *e = &d->vals.e[i];
            const struct ksp_order_entry *o = ksp_order_find(&d->ovals, e->kind,
                                                             e->name);
            if (!o || strcmp(o->name, e->name) ||
                (!o->anchor != !e->anchor) ||
                (o->anchor && strcmp(o->anchor, e->anchor)))
                return 1;
        }
        return 0;
    }
    {
        size_t len;
        char *b = ksp_order_doc_bytes(d, &len);
        ch = len != d->origlen || (len && memcmp(b, d->orig, len));
        sfree(b);
    }
    return ch;
}

/* ======================================================================
 * The store
 * ====================================================================== */

void ksp_order_store_free(struct ksp_order_store *st)
{
    if (!st)
        return;
    sfree(st->dir);
    sfree(st->regbase);
    memset(st, 0, sizeof(*st));
}

/* The registry root of a store: HKEY_CURRENT_USER unless the unit test
 * hands over a private hive. */
static HKEY kso_root(const struct ksp_order_store *st)
{
    return st && st->regroot ? (HKEY)st->regroot : HKEY_CURRENT_USER;
}

/* A folder's key below SessionOrder: the folder escaped as Sessions
 * escapes a name, the root as the escape of "\". snewn'd. */
static char *kso_reg_subkey(const char *folder)
{
    strbuf *sb = strbuf_new();
    if (ksp_folder_is_root(folder)) {
        put_dataz(sb, KSP_ORDER_REGROOT);
    } else {
        char *norm = ksp_normalise(folder);
        if (*norm)
            escape_registry_key(norm, sb);
        else
            put_dataz(sb, KSP_ORDER_REGROOT);
        sfree(norm);
    }
    return strbuf_to_str(sb);
}

char *ksp_order_location(const struct ksp_order_store *st, const char *folder)
{
    char *r;
    if (!st)
        return NULL;
    if (st->folder_store) {
        char *norm = ksp_folder_is_root(folder) ? dupstr("") : ksp_normalise(folder);
        if (!*norm) {
            r = dupprintf("%s\\%s", st->dir ? st->dir : ".", KSP_ORDER_FILE);
        } else {
            char *rel = ksp_path_to_relfile(norm, "");
            r = dupprintf("%s\\%s\\%s", st->dir ? st->dir : ".", rel, KSP_ORDER_FILE);
            sfree(rel);
        }
        sfree(norm);
    } else {
        char *sub = kso_reg_subkey(folder);
        r = dupprintf("%s\\%s\\%s", st->regbase ? st->regbase : "",
                      KSP_ORDER_REGKEY, sub);
        sfree(sub);
    }
    return r;
}

/* A file whole; NULL when absent or not readable (a damaged or huge file
 * reads as no file). */
#define KSO_FILE_MAX (4 * 1024 * 1024)
static char *kso_read_file(const char *path, size_t *len)
{
    HANDLE h;
    LARGE_INTEGER sz;
    char *buf;
    DWORD got = 0;
    size_t have = 0;
    *len = 0;
    h = CreateFileA(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > KSO_FILE_MAX) {
        CloseHandle(h);
        return NULL;
    }
    buf = snewn((size_t)sz.QuadPart + 1, char);
    while (have < (size_t)sz.QuadPart &&
           ReadFile(h, buf + have, (DWORD)((size_t)sz.QuadPart - have), &got, NULL) &&
           got > 0)
        have += got;
    CloseHandle(h);
    buf[have] = '\0';
    *len = have;
    return buf;
}

/* The values of a SessionOrder key: "s:<name>" / "f:<name>" REG_SZ, the
 * rest ignored. *stamp = its last-write time; 0 when there is no key. */
static void kso_read_values(HKEY root, const char *key,
                            struct ksp_order_list *out,
                            unsigned long long *stamp)
{
    HKEY k;
    DWORD nvals = 0, maxname = 0, maxdata = 0, i;
    FILETIME ft;
    char *name, *data;

    ksp_order_list_init(out);
    if (stamp)
        *stamp = 0;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    memset(&ft, 0, sizeof(ft));
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, &nvals,
                         &maxname, &maxdata, NULL, &ft) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return;
    }
    if (stamp)
        *stamp = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    name = snewn(maxname + 2, char);
    data = snewn(maxdata + 2, char);
    for (i = 0; i < nvals; i++) {
        DWORD nl = maxname + 1, dl = maxdata, type = 0;
        if (RegEnumValueA(k, i, name, &nl, NULL, &type, (BYTE *)data,
                          &dl) != ERROR_SUCCESS)
            continue;
        if (type != REG_SZ || nl < 3 || name[1] != ':' ||
            (name[0] != KSP_ORDER_SESSION && name[0] != KSP_ORDER_FOLDER))
            continue;
        if (dl > maxdata)
            dl = maxdata;
        data[dl] = '\0';
        ksp_order_list_add(out, name[0], name + 2, data[0] ? data : NULL);
    }
    sfree(data);
    sfree(name);
    RegCloseKey(k);
    if (out->n > 1)
        qsort(out->e, out->n, sizeof(*out->e), kso_entry_cmp);
}

static unsigned long long kso_file_stamp(const char *path, unsigned long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    *size = 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
        return 0;
    *size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
           fa.ftLastWriteTime.dwLowDateTime;
}

static unsigned long long kso_key_stamp(HKEY root, const char *key)
{
    HKEY k;
    FILETIME ft;
    unsigned long long r = 0;
    if (RegOpenKeyExA(root, key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, &ft) == ERROR_SUCCESS)
        r = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    RegCloseKey(k);
    return r;
}

/* ---- the per-process cache ---- */
struct kso_cached {
    char *loc;
    int is_reg;
    unsigned long long stamp, size;
    struct ksp_order_list list;
};
static struct kso_cached *kso_cache;
static int kso_ncache;
static size_t kso_cachecap;
#define KSO_CACHE_MAX 512

void ksp_order_cache_clear(void)
{
    int i;
    for (i = 0; i < kso_ncache; i++) {
        sfree(kso_cache[i].loc);
        ksp_order_list_free(&kso_cache[i].list);
    }
    kso_ncache = 0;
}

const struct ksp_order_list *ksp_order_load(const struct ksp_order_store *st,
                                            const char *folder)
{
    char *loc;
    int i, is_reg;
    unsigned long long stamp, size = 0;
    struct kso_cached *c = NULL;

    if (!st)
        return NULL;
    loc = ksp_order_location(st, folder);
    is_reg = !st->folder_store;
    stamp = is_reg ? kso_key_stamp(kso_root(st), loc) : kso_file_stamp(loc, &size);
    for (i = 0; i < kso_ncache; i++)
        if (kso_cache[i].is_reg == is_reg && !stricmp(kso_cache[i].loc, loc)) {
            c = &kso_cache[i];
            break;
        }
    if (c && c->stamp == stamp && c->size == size) {
        sfree(loc);
        return &c->list;
    }
    if (!c) {
        if (kso_ncache >= KSO_CACHE_MAX)
            ksp_order_cache_clear();
        sgrowarray(kso_cache, kso_cachecap, kso_ncache);
        c = &kso_cache[kso_ncache++];
        memset(c, 0, sizeof(*c));
        c->loc = loc;
        c->is_reg = is_reg;
        loc = NULL;
    } else {
        ksp_order_list_free(&c->list);
    }
    if (is_reg) {
        kso_read_values(kso_root(st), c->loc, &c->list, &c->stamp);
        c->size = 0;
        c->stamp = stamp ? stamp : c->stamp;
    } else {
        size_t len = 0;
        char *buf = stamp ? kso_read_file(c->loc, &len) : NULL;
        struct ksp_order_doc *d = ksp_order_doc_text(buf ? buf : "", buf ? len : 0);
        ksp_order_doc_entries(d, &c->list);
        ksp_order_doc_free(d);
        sfree(buf);
        c->stamp = stamp;
        c->size = size;
    }
    sfree(loc);
    return &c->list;
}

struct ksp_order_doc *ksp_order_open(const struct ksp_order_store *st,
                                     const char *folder)
{
    char *loc;
    struct ksp_order_doc *d;
    if (!st)
        return ksp_order_doc_text("", 0);
    loc = ksp_order_location(st, folder);
    if (st->folder_store) {
        size_t len = 0;
        char *buf = kso_read_file(loc, &len);
        d = ksp_order_doc_text(buf ? buf : "", buf ? len : 0);
        sfree(buf);
    } else {
        struct ksp_order_list v;
        kso_read_values(kso_root(st), loc, &v, NULL);
        d = ksp_order_doc_values(&v);
        ksp_order_list_free(&v);
    }
    sfree(loc);
    return d;
}

/* The directories above a file, made one by one (kageant never writes;
 * kitty.exe passes its own maker with the store's rights). */
static void kso_make_dirs(const char *filepath)
{
    char *p = dupstr(filepath), *s;
    for (s = p; *s; s++) {
        if ((*s == '\\' || *s == '/') && s > p && s[-1] != ':' &&
            !(s == p + 1 && p[0] == '\\')) {
            char c = *s;
            *s = '\0';
            CreateDirectoryA(p, NULL);
            *s = c;
        }
    }
    sfree(p);
}

/* The directories a file leaves empty go too, up to the session
 * directory (RemoveDirectory refuses one with anything in it). */
static void kso_prune_dirs(const struct ksp_order_store *st, const char *filepath)
{
    char *p = dupstr(filepath), *bs;
    size_t rootlen = st->dir ? strlen(st->dir) : 0;
    while ((bs = strrchr(p, '\\')) != NULL) {
        *bs = '\0';
        if (strlen(p) <= rootlen || !rootlen || strnicmp(p, st->dir, rootlen))
            break;
        if (!RemoveDirectoryA(p))
            break;
    }
    sfree(p);
}

static int kso_write_values(HKEY root, const char *key,
                            const struct ksp_order_doc *d)
{
    HKEY k;
    LONG r;
    int i;
    if (d->vals.n == 0) {
        r = RegDeleteKeyA(root, key);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
            SetLastError(r);
            return 0;
        }
        return 1;
    }
    r = RegCreateKeyExA(root, key, 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) {
        SetLastError(r);
        return 0;
    }
    for (i = 0; i < d->ovals.n; i++) {
        const struct ksp_order_entry *o = &d->ovals.e[i];
        if (!ksp_order_find(&d->vals, o->kind, o->name)) {
            char *vn = dupprintf("%c:%s", o->kind, o->name);
            r = RegDeleteValueA(k, vn);
            sfree(vn);
            if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
                break;
            r = ERROR_SUCCESS;
        }
    }
    for (i = 0; r == ERROR_SUCCESS && i < d->vals.n; i++) {
        const struct ksp_order_entry *e = &d->vals.e[i];
        const struct ksp_order_entry *o = ksp_order_find(&d->ovals, e->kind, e->name);
        const char *data = e->anchor ? e->anchor : "";
        char *vn;
        if (o && !strcmp(o->name, e->name) && !o->anchor == !e->anchor &&
            (!o->anchor || !strcmp(o->anchor, e->anchor)))
            continue;
        vn = dupprintf("%c:%s", e->kind, e->name);
        r = RegSetValueExA(k, vn, 0, REG_SZ, (const BYTE *)data,
                           (DWORD)strlen(data) + 1);
        sfree(vn);
    }
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) {
        SetLastError(r);
        return 0;
    }
    return 1;
}

int ksp_order_commit(const struct ksp_order_store *st, const char *folder,
                     struct ksp_order_doc *d)
{
    char *loc;
    int ok = 1;
    if (!st || !d)
        return 1;
    if (kitty_store_readonly())
        return 1;
    if (!ksp_order_doc_changed(d))
        return 1;
    loc = ksp_order_location(st, folder);
    if (d->is_reg) {
        ok = kso_write_values(kso_root(st), loc, d);
    } else if (ksp_order_doc_empty(d)) {
        if (!DeleteFileA(loc)) {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
                ok = 0;
        }
    } else {
        size_t len;
        char *b = ksp_order_doc_bytes(d, &len);
        if (st->make_dirs)
            st->make_dirs(loc);
        else
            kso_make_dirs(loc);
        ok = kitty_write_file_whole(loc, b, len);
        sfree(b);
    }
    sfree(loc);
    ksp_order_cache_clear();
    return ok;
}

void ksp_order_sort_level(const struct ksp_order_store *st, const char *folder,
                          char kind, char *const *names, int n, int *perm,
                          unsigned char *pinned)
{
    int i;
    if (n <= 0)
        return;
    if (!st) {
        for (i = 0; i < n; i++) {
            perm[i] = i;
            if (pinned)
                pinned[i] = 0;
        }
        return;
    }
    ksp_order_apply(ksp_order_load(st, folder), kind, names, n, perm, pinned);
}

void ksp_order_shown_rows(const struct ksp_order_store *st,
                          struct ksp_shown_row *rows, int n,
                          unsigned char *pinned)
{
    unsigned char *done, *pin;
    int *slots, *perm, i, j, k;
    char **names;
    struct ksp_shown_row *copy;

    if (pinned)
        for (i = 0; i < n; i++)
            pinned[i] = 0;
    if (!st || n <= 0)
        return;
    done = snewn(n, unsigned char);
    slots = snewn(n, int);
    perm = snewn(n, int);
    pin = snewn(n, unsigned char);
    names = snewn(n, char *);
    copy = snewn(n, struct ksp_shown_row);
    for (i = 0; i < n; i++)
        done[i] = !strcmp(rows[i].id, "Default Settings");
    for (i = 0; i < n; i++) {
        const char *fi;
        int ns = 0;
        if (done[i])
            continue;
        fi = rows[i].folder;
        for (j = i; j < n; j++)
            if (!done[j] && ksp_folder_same(fi, rows[j].folder)) {
                done[j] = 1;
                slots[ns] = j;
                names[ns] = (char *)ksp_leaf(rows[j].id);
                copy[ns] = rows[j];
                ns++;
            }
        ksp_order_sort_level(st, fi, KSP_ORDER_SESSION, names, ns, perm, pin);
        for (k = 0; k < ns; k++) {
            rows[slots[k]] = copy[perm[k]];
            if (pinned)
                pinned[slots[k]] = pin[perm[k]];
        }
    }
    sfree(copy);
    sfree(names);
    sfree(pin);
    sfree(perm);
    sfree(slots);
    sfree(done);
}

/* ======================================================================
 * The store changes under the order
 * ====================================================================== */

int ksp_order_forget(const struct ksp_order_store *st, const char *folder,
                     char kind, const char *name)
{
    struct ksp_order_doc *d;
    struct ksp_order_list cur;
    struct ksp_order_changes ch;
    const struct ksp_order_entry *e;
    int i, ok;

    if (!st || !name)
        return 1;
    d = ksp_order_open(st, folder);
    ksp_order_doc_entries(d, &cur);
    memset(&ch, 0, sizeof(ch));
    e = ksp_order_find(&cur, kind, name);
    if (e)
        kso_change_add(&ch, kind, name, NULL, 1, 0);
    for (i = 0; e && i < cur.n; i++) {
        const struct ksp_order_entry *f = &cur.e[i];
        if (f->kind == kind && f->anchor && !stricmp(f->anchor, name) &&
            stricmp(f->name, name) && ksp_order_find(&cur, kind, f->name) == f)
            kso_change_add(&ch, kind, f->name, e->anchor, 0, 0);
    }
    if (ch.n)
        ksp_order_doc_change(d, kind, NULL, 0, &ch);
    ok = ksp_order_commit(st, folder, d);
    ksp_order_changes_free(&ch);
    ksp_order_list_free(&cur);
    ksp_order_doc_free(d);
    return ok;
}

int ksp_order_rename(const struct ksp_order_store *st, const char *folder,
                     char kind, const char *from, const char *to)
{
    struct ksp_order_doc *d;
    int ok;
    if (!st || !from || !to || !strcmp(from, to))
        return 1;
    d = ksp_order_open(st, folder);
    ksp_order_doc_rename(d, kind, from, to);
    ok = ksp_order_commit(st, folder, d);
    ksp_order_doc_free(d);
    return ok;
}

int ksp_order_reset(const struct ksp_order_store *st, const char *folder)
{
    struct ksp_order_doc *d;
    int ok;
    if (!st)
        return 1;
    d = ksp_order_open(st, folder);
    ksp_order_doc_reset(d);
    ok = ksp_order_commit(st, folder, d);
    ksp_order_doc_free(d);
    return ok;
}

/* ---- a folder's places following its path ---- */

/* Folder store: every .kittyorder below `dir` (dir itself included), as
 * paths relative to `dir` ("" = dir's own). Reparse points are not
 * followed. */
static void kso_walk_orders(const char *dir, const char *rel, int depth,
                            char ***out, int *n, size_t *cap)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *pat, *full;
    full = *rel ? dupprintf("%s\\%s", dir, rel) : dupstr(dir);
    {
        char *f = dupprintf("%s\\%s", full, KSP_ORDER_FILE);
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) {
            sgrowarray(*out, *cap, *n);
            (*out)[(*n)++] = dupstr(rel);
        }
        sfree(f);
    }
    if (depth >= KSP_WALK_MAXDEPTH) {
        sfree(full);
        return;
    }
    pat = dupprintf("%s\\*", full);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                fd.cFileName[0] == '.')
                continue;
            {
                char *sub = *rel ? dupprintf("%s\\%s", rel, fd.cFileName)
                                 : dupstr(fd.cFileName);
                kso_walk_orders(dir, sub, depth + 1, out, n, cap);
                sfree(sub);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sfree(pat);
    sfree(full);
}

/* The directory of a folder (its .kittyorder's place without the name). */
static char *kso_folder_dir(const struct ksp_order_store *st, const char *folder)
{
    char *loc = ksp_order_location(st, folder);
    char *bs = strrchr(loc, '\\');
    if (bs)
        *bs = '\0';
    return loc;
}

static int kso_folder_moved_files(const struct ksp_order_store *st,
                                  const char *from, const char *to, int merged)
{
    char *fromdir = kso_folder_dir(st, from), *todir = to ? kso_folder_dir(st, to) : NULL;
    char **rels = NULL;
    int n = 0, i, ok = 1;
    size_t cap = 0;
    kso_walk_orders(fromdir, "", 0, &rels, &n, &cap);
    for (i = 0; i < n; i++) {
        char *src = *rels[i] ? dupprintf("%s\\%s\\%s", fromdir, rels[i], KSP_ORDER_FILE)
                             : dupprintf("%s\\%s", fromdir, KSP_ORDER_FILE);
        char *dst = NULL;
        if (todir && !(merged && !*rels[i]))
            dst = *rels[i] ? dupprintf("%s\\%s\\%s", todir, rels[i], KSP_ORDER_FILE)
                           : dupprintf("%s\\%s", todir, KSP_ORDER_FILE);
        if (dst && !stricmp(src, dst)) {
            /* a change of case only: the directory is the same one */
            sfree(src);
            sfree(dst);
            continue;
        }
        if (dst && GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) {
            if (st->make_dirs)
                st->make_dirs(dst);
            else
                kso_make_dirs(dst);
            if (!MoveFileExA(src, dst, MOVEFILE_COPY_ALLOWED))
                ok = 0;
        } else if (!DeleteFileA(src)) {
            ok = 0;
        }
        kso_prune_dirs(st, src);
        sfree(src);
        sfree(dst);
    }
    for (i = 0; i < n; i++)
        sfree(rels[i]);
    sfree(rels);
    sfree(fromdir);
    sfree(todir);
    return ok;
}

static int kso_folder_moved_keys(const struct ksp_order_store *st,
                                 const char *from, const char *to, int merged)
{
    char *base = dupprintf("%s\\%s", st->regbase ? st->regbase : "", KSP_ORDER_REGKEY);
    HKEY k, root = kso_root(st);
    char **subs = NULL;
    int n = 0, i, ok = 1;
    size_t cap = 0;
    DWORD idx, maxsub = 0;
    if (RegOpenKeyExA(root, base, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        char *buf;
        RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, &maxsub, NULL, NULL, NULL,
                         NULL, NULL, NULL);
        buf = snewn(maxsub + 2, char);
        for (idx = 0; ; idx++) {
            DWORD bl = maxsub + 1;
            if (RegEnumKeyExA(k, idx, buf, &bl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            sgrowarray(subs, cap, n);
            subs[n++] = dupstr(buf);
        }
        sfree(buf);
        RegCloseKey(k);
    }
    for (i = 0; i < n; i++) {
        strbuf *un = strbuf_new();
        char *f, *src, *dst = NULL;
        unescape_registry_key(subs[i], un);
        f = strbuf_to_str(un);
        if (!stricmp(subs[i], KSP_ORDER_REGROOT) || !*f ||
            !ksp_folder_within(f, from)) {
            sfree(f);
            continue;
        }
        src = dupprintf("%s\\%s", base, subs[i]);
        if (to && !(merged && ksp_folder_same(f, from))) {
            char *nf = ksp_folder_moved_path(f, from, to);
            dst = ksp_order_location(st, nf);
            sfree(nf);
        }
        if (dst && !stricmp(src, dst) && !strcmp(src, dst)) {
            /* nothing moves */
        } else if (dst) {
            HKEY t;
            int taken = stricmp(src, dst) &&
                RegOpenKeyExA(root, dst, 0, KEY_READ, &t) == ERROR_SUCCESS;
            if (taken) {
                RegCloseKey(t);
                if (!ksp_reg_delete_tree(root, src))
                    ok = 0;
            } else {
                char *leftover = NULL;
                if (!ksp_reg_move_tree(root, src, dst, &leftover))
                    ok = 0;
                sfree(leftover);
            }
        } else if (!ksp_reg_delete_tree(root, src)) {
            ok = 0;
        }
        sfree(dst);
        sfree(src);
        sfree(f);
    }
    for (i = 0; i < n; i++)
        sfree(subs[i]);
    sfree(subs);
    sfree(base);
    return ok;
}

int ksp_order_folder_moved(const struct ksp_order_store *st, const char *from,
                           const char *to, int merged)
{
    char *pfrom, *pto = NULL;
    int ok = 1;
    if (!st || ksp_folder_is_root(from) || kitty_store_readonly())
        return 1;
    if (to && ksp_folder_is_root(to) && !merged)
        to = NULL;                      /* cannot become the root itself */
    /* its own entry in the folder above it */
    pfrom = ksp_folder_parent(from);
    if (to && !merged)
        pto = ksp_folder_parent(to);
    if (pto && ksp_folder_same(pfrom, pto))
        ok &= ksp_order_rename(st, pfrom, KSP_ORDER_FOLDER, ksp_leaf(from),
                               ksp_leaf(to));
    else
        ok &= ksp_order_forget(st, pfrom, KSP_ORDER_FOLDER, ksp_leaf(from));
    sfree(pto);
    sfree(pfrom);
    /* its own places and those of the folders below it */
    if (st->folder_store)
        ok &= kso_folder_moved_files(st, from, to, merged);
    else
        ok &= kso_folder_moved_keys(st, from, to, merged);
    ksp_order_cache_clear();
    return ok;
}

/* ---- carried to another store (Take folder store, the portable copy) ---- */

/* Every level that has places in `st`: its folder path (NULL = the root). */
static char **kso_levels(const struct ksp_order_store *st, int *n)
{
    char **out = NULL;
    size_t cap = 0;
    int i;
    *n = 0;
    if (st->folder_store) {
        char **rels = NULL;
        int nr = 0;
        size_t rc = 0;
        if (!st->dir)
            return NULL;
        kso_walk_orders(st->dir, "", 0, &rels, &nr, &rc);
        for (i = 0; i < nr; i++) {
            strbuf *sb = strbuf_new();
            const char *p = rels[i];
            while (*p) {
                const char *e = strchr(p, '\\');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                char *comp = dupprintf("%.*s", (int)len, p);
                char *plain = ksp_component_unmunge(comp);
                if (sb->len)
                    put_byte(sb, '\\');
                put_dataz(sb, plain);
                sfree(plain);
                sfree(comp);
                p += len;
                while (*p == '\\')
                    p++;
            }
            sgrowarray(out, cap, *n);
            if (sb->len) {
                out[(*n)++] = strbuf_to_str(sb);
            } else {
                strbuf_free(sb);
                out[(*n)++] = NULL;     /* the top level */
            }
            sfree(rels[i]);
        }
        sfree(rels);
    } else {
        char *base = dupprintf("%s\\%s", st->regbase ? st->regbase : "",
                               KSP_ORDER_REGKEY);
        HKEY k;
        DWORD idx, maxsub = 0;
        if (RegOpenKeyExA(kso_root(st), base, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            char *buf;
            RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, &maxsub, NULL, NULL,
                             NULL, NULL, NULL, NULL);
            buf = snewn(maxsub + 2, char);
            for (idx = 0; ; idx++) {
                DWORD bl = maxsub + 1;
                strbuf *un;
                if (RegEnumKeyExA(k, idx, buf, &bl, NULL, NULL, NULL,
                                  NULL) != ERROR_SUCCESS)
                    break;
                sgrowarray(out, cap, *n);
                if (!stricmp(buf, KSP_ORDER_REGROOT)) {
                    out[(*n)++] = NULL;
                    continue;
                }
                un = strbuf_new();
                unescape_registry_key(buf, un);
                out[(*n)++] = strbuf_to_str(un);
            }
            sfree(buf);
            RegCloseKey(k);
        }
        sfree(base);
    }
    return out;
}

int ksp_order_copy_all(const struct ksp_order_store *from,
                       const struct ksp_order_store *to)
{
    char **levels;
    int n, i, j, failed = 0;
    if (!from || !to || kitty_store_readonly())
        return 0;
    levels = kso_levels(from, &n);
    for (i = 0; i < n; i++) {
        struct ksp_order_doc *src = ksp_order_open(from, levels[i]);
        struct ksp_order_doc *dst = ksp_order_open(to, levels[i]);
        struct ksp_order_list se, de;
        int kk;
        ksp_order_doc_entries(src, &se);
        ksp_order_doc_entries(dst, &de);
        for (kk = 0; kk < 2; kk++) {
            char kind = kk ? KSP_ORDER_FOLDER : KSP_ORDER_SESSION;
            struct ksp_order_changes ch;
            memset(&ch, 0, sizeof(ch));
            for (j = 0; j < se.n; j++) {
                const struct ksp_order_entry *e = &se.e[j];
                if (e->kind != kind || ksp_order_find(&se, kind, e->name) != e ||
                    ksp_order_find(&de, kind, e->name))
                    continue;           /* a place of its own there stays */
                kso_change_add(&ch, kind, e->name, e->anchor, 0, 1);
            }
            if (ch.n)
                ksp_order_doc_change(dst, kind, NULL, 0, &ch);
            ksp_order_changes_free(&ch);
        }
        if (!ksp_order_commit(to, levels[i], dst))
            failed++;
        ksp_order_list_free(&de);
        ksp_order_list_free(&se);
        ksp_order_doc_free(dst);
        ksp_order_doc_free(src);
        sfree(levels[i]);
    }
    sfree(levels);
    ksp_order_cache_clear();
    return failed;
}
