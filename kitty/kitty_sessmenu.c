/*
 * kitty_sessmenu.c - a saved-session menu laid out as the folder tree
 * (hknet/KiTTY#55). See kitty_sessmenu.h. Pure apart from reading a session
 * file's Folder value; compiled into kageant and its unit test, neither of
 * which links the settings library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty_storage.h"      /* ksf_unmunge */
#include "kitty_sessionpath.h"
#include "kitty_sessmenu.h"

/* ---- the tree ---- */

struct ksm_sess {
    const char *leaf;
    const char *id;
    int idx;
};

struct ksm_node {
    char *name;
    struct ksm_node **kids;
    int nkids;
    size_t kcap;
    struct ksm_sess *sess;
    int nsess;
    size_t scap;
};

static void ksm_node_free(struct ksm_node *nd)
{
    int i;
    for (i = 0; i < nd->nkids; i++)
        ksm_node_free(nd->kids[i]);
    sfree(nd->kids);
    sfree(nd->sess);
    sfree(nd->name);
    sfree(nd);
}

/* The child folder `name` of `nd`, made when new. Both stores ignore case,
 * so "Linux" and "linux" are one folder; it shows the spelling that sorts
 * first by byte, whatever order the sessions came in. */
static struct ksm_node *ksm_child(struct ksm_node *nd, const char *name,
                                  size_t len)
{
    struct ksm_node *k;
    int i;
    for (i = 0; i < nd->nkids; i++) {
        k = nd->kids[i];
        if (strlen(k->name) == len && !strnicmp(k->name, name, len)) {
            if (strncmp(name, k->name, len) < 0)
                memcpy(k->name, name, len);
            return k;
        }
    }
    k = snew(struct ksm_node);
    memset(k, 0, sizeof(*k));
    k->name = dupprintf("%.*s", (int)len, name);
    sgrowarray(nd->kids, nd->kcap, nd->nkids);
    nd->kids[nd->nkids++] = k;
    return k;
}

static int ksm_name_cmp(const char *a, const char *b)
{
    int c = ksp_natcasecmp(a, b);
    if (!c)
        c = stricmp(a, b);
    if (!c)
        c = strcmp(a, b);
    return c;
}

static int ksm_kid_qcmp(const void *av, const void *bv)
{
    const struct ksm_node *a = *(const struct ksm_node *const *)av;
    const struct ksm_node *b = *(const struct ksm_node *const *)bv;
    return ksm_name_cmp(a->name, b->name);
}

static int ksm_sess_qcmp(const void *av, const void *bv)
{
    const struct ksm_sess *a = av, *b = bv;
    int c = ksm_name_cmp(a->leaf, b->leaf);
    if (!c)
        c = strcmp(a->id, b->id);
    if (!c)
        c = (a->idx > b->idx) - (a->idx < b->idx);
    return c;
}

struct ksm_out {
    struct ksm_entry *e;
    int n;
    size_t cap;
};

static void ksm_emit(struct ksm_out *o, int kind, const char *label, int idx)
{
    sgrowarray(o->e, o->cap, o->n);
    o->e[o->n].kind = kind;
    o->e[o->n].label = label ? dupstr(label) : NULL;
    o->e[o->n].idx = idx;
    o->n++;
}

static void ksm_walk(struct ksm_node *nd, struct ksm_out *o)
{
    int i;
    if (nd->nkids > 1)
        qsort(nd->kids, nd->nkids, sizeof(*nd->kids), ksm_kid_qcmp);
    if (nd->nsess > 1)
        qsort(nd->sess, nd->nsess, sizeof(*nd->sess), ksm_sess_qcmp);
    for (i = 0; i < nd->nkids; i++) {
        ksm_emit(o, KSM_FOLDER_OPEN, nd->kids[i]->name, -1);
        ksm_walk(nd->kids[i], o);
        ksm_emit(o, KSM_FOLDER_CLOSE, NULL, -1);
    }
    for (i = 0; i < nd->nsess; i++)
        ksm_emit(o, KSM_SESSION, nd->sess[i].leaf, nd->sess[i].idx);
}

struct ksm_entry *ksm_build(char *const *ids, char *const *folders, int n,
                            int *nentries)
{
    struct ksm_node *root = snew(struct ksm_node);
    struct ksm_out o;
    int i;

    memset(root, 0, sizeof(*root));
    memset(&o, 0, sizeof(o));
    for (i = 0; i < n; i++) {
        const char *id = ids[i];
        char *fld;
        struct ksm_node *nd = root;
        const char *p;
        if (!id || !*id || !strcmp(id, "Default Settings"))
            continue;
        /* The path wins; a bare name goes by its Folder value. */
        if (strchr(id, '\\'))
            fld = ksp_folder_of(id);
        else if (folders && !ksp_folder_is_root(folders[i]))
            fld = ksp_normalise(folders[i]);
        else
            fld = NULL;
        for (p = fld ? fld : ""; *p; ) {
            const char *e = strchr(p, '\\');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len)
                nd = ksm_child(nd, p, len);
            p += len;
            if (*p == '\\')
                p++;
        }
        sfree(fld);
        if (!*ksp_leaf(id))
            continue;                  /* "Linux\" names no session */
        sgrowarray(nd->sess, nd->scap, nd->nsess);
        nd->sess[nd->nsess].leaf = ksp_leaf(id);
        nd->sess[nd->nsess].id = id;
        nd->sess[nd->nsess].idx = i;
        nd->nsess++;
    }
    ksm_walk(root, &o);
    ksm_node_free(root);
    *nentries = o.n;
    return o.e;
}

void ksm_free(struct ksm_entry *e, int n)
{
    int i;
    for (i = 0; i < n; i++)
        sfree(e[i].label);
    sfree(e);
}

/* ---- a session file's Folder value ----
 * The line forms of the folder store's reader (ksf_load): the first delimiter
 * after the key decides - '=' is KiTTY++'s "Key=value", '\' is classic
 * KiTTY's "Key\value\". Values are %xx escaped in both, so a '\' inside a
 * KiTTY++ value is always "%5C" and never taken for the classic form. The
 * last Folder line wins, as in ksf_load. */
static char *ksm_line_folder(const char *line, size_t n)
{
    static const char key[] = "Folder";
    const size_t kl = sizeof(key) - 1;
    const char *eq, *bs, *v;
    size_t vl;
    char *raw, *out;
    if (n <= kl || memcmp(line, key, kl))
        return NULL;
    eq = memchr(line, '=', n);
    bs = memchr(line, '\\', n);
    if (eq && (!bs || eq < bs)) {
        if (eq != line + kl)
            return NULL;
        v = eq + 1;
        vl = n - (size_t)(v - line);
    } else if (bs) {
        if (bs != line + kl)
            return NULL;
        v = bs + 1;
        vl = n - (size_t)(v - line);
        if (vl && v[vl - 1] == '\\')
            vl--;                      /* the closing delimiter */
    } else {
        return NULL;
    }
    raw = dupprintf("%.*s", (int)vl, v);
    out = ksf_unmunge(raw);
    sfree(raw);
    return out;
}

char *ksm_text_folder(const char *buf, size_t len)
{
    char *found = NULL;
    size_t i = 0;
    if (!buf)
        return NULL;
    while (i < len) {
        size_t s = i;
        char *f;
        while (i < len && buf[i] != '\n' && buf[i] != '\r')
            i++;
        if ((f = ksm_line_folder(buf + s, i - s)) != NULL) {
            sfree(found);
            found = f;
        }
        while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
            i++;
    }
    return found;
}

static unsigned long ksm_nreads;

char *ksm_file_folder(const char *path)
{
    FILE *fp;
    char line[1024];
    char *found = NULL;
    int at_line_start = 1;
    if (!path || !(fp = fopen(path, "rb")))
        return NULL;
    ksm_nreads++;
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        int whole = (n > 0 && line[n - 1] == '\n');
        if (at_line_start) {
            char *f;
            size_t m = n;
            while (m && (line[m - 1] == '\n' || line[m - 1] == '\r'))
                m--;
            /* A Folder line longer than the buffer is not a folder anyone
             * typed; only a whole line is read. */
            if (whole && (f = ksm_line_folder(line, m)) != NULL) {
                sfree(found);
                found = f;
            }
        }
        at_line_start = whole;
    }
    fclose(fp);
    return found;
}

unsigned long ksm_folder_reads(void)
{
    return ksm_nreads;
}

/* ---- the Folder cache ---- */

struct ksm_fentry {
    char *path;
    char *folder;               /* NULL: the file has none */
    unsigned long long size, mtime;
    int settled;
    unsigned long seen;
};

#define KSM_SETTLE (3ULL * 10000000ULL)   /* 100 ns units */

static tree234 *ksm_fcache;
static unsigned long ksm_fgen = 1;

static int ksm_fcmp(void *av, void *bv)
{
    return stricmp(((struct ksm_fentry *)av)->path,
                   ((struct ksm_fentry *)bv)->path);
}

char *ksm_file_folder_cached(const char *path, unsigned long long size,
                             unsigned long long mtime)
{
    struct ksm_fentry key, *e;
    if (!path)
        return NULL;
    if (!ksm_fcache)
        ksm_fcache = newtree234(ksm_fcmp);
    key.path = (char *)path;
    e = find234(ksm_fcache, &key, NULL);
    /* A file written in the last few seconds is read again every time: its
     * time may not move on a second write (see ksp_file_verdict). */
    if (!e || !e->settled || e->size != size || e->mtime != mtime) {
        FILETIME ft;
        unsigned long long now;
        if (!e) {
            e = snew(struct ksm_fentry);
            e->path = dupstr(path);
            e->folder = NULL;
            add234(ksm_fcache, e);
        }
        sfree(e->folder);
        e->folder = ksm_file_folder(path);
        e->size = size;
        e->mtime = mtime;
        GetSystemTimeAsFileTime(&ft);
        now = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        e->settled = mtime < now && now - mtime >= KSM_SETTLE;
    }
    e->seen = ksm_fgen;
    return e->folder ? dupstr(e->folder) : NULL;
}

void ksm_folder_cache_begin(void)
{
    ksm_fgen++;
}

void ksm_folder_cache_end(void)
{
    int i;
    if (!ksm_fcache)
        return;
    for (i = count234(ksm_fcache) - 1; i >= 0; i--) {
        struct ksm_fentry *e = index234(ksm_fcache, i);
        if (e->seen != ksm_fgen) {
            delpos234(ksm_fcache, i);
            sfree(e->path);
            sfree(e->folder);
            sfree(e);
        }
    }
}

/* ---- the folder store's rows ---- */

int ksm_folder_store_rows(const char *dir, const char *suffix,
                          char ***ids, char ***folders)
{
    struct ksp_store_file *v;
    int i, n = 0;
    *ids = NULL;
    *folders = NULL;
    v = ksp_walk_store(dir, suffix, NULL, &n);
    if (n <= 0) {
        ksp_walk_store_free(v, n);
        return 0;
    }
    *ids = snewn(n, char *);
    *folders = snewn(n, char *);
    ksm_folder_cache_begin();
    for (i = 0; i < n; i++) {
        (*ids)[i] = v[i].id;             /* handed over */
        v[i].id = NULL;
        /* Only a session stored by its bare name has a Folder that counts:
         * the path wins, and is never read for it. */
        (*folders)[i] = strchr((*ids)[i], '\\') ? NULL :
            ksm_file_folder_cached(v[i].path, v[i].size, v[i].mtime);
    }
    ksm_folder_cache_end();
    ksp_walk_store_free(v, n);
    return n;
}

void ksm_rows_free(char **ids, char **folders, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (ids)
            sfree(ids[i]);
        if (folders)
            sfree(folders[i]);
    }
    sfree(ids);
    sfree(folders);
}
