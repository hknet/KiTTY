/*
 * test_sessorder.c - the manual order of folders and sessions (Manage
 * Sessions' drag and drop): kitty/kitty_sessorder.c.
 *
 * Part one drives the resolver: chains, a missing anchor, a cycle, several
 * "first", names without case, a level with only stored places, two items of
 * one shown name. Part two the planner (a drop between two rows, to the top,
 * a block of three, the item below the drop re-anchored, a forgotten item).
 * Part three parses .kittyorder texts (BOM, CRLF, comments, the two-field
 * list form, anchors, unknown names, junk lines). Part four edits texts in
 * place against golden before/after texts: move up, move down, to the top,
 * onto an item with no place, rename, forget, Reset Order, a drop that
 * changes nothing; comments, CRLF and the BOM untouched. Part five runs the
 * store: a folder store under %TEMP% and a registry key tree in a private
 * hive file (RegLoadAppKey) - never the user's registry, never a real
 * session - load, commit, the cache, a folder moved and deleted.
 *
 * Linked as kageant links the order: no settings library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty/kitty_sessionpath.h"
#include "kitty/kitty_sessorder.h"
#include "kitty/kitty_inifile.h"

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
void nonfatal(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int checks, failures;
static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}
static void head(const char *s)
{
    printf("\n== %s\n", s);
}

/* "a b* c": the level in the order shown, '*' after an item with a place. */
static char *shown(const struct ksp_order_list *l, char kind,
                   char *const *names, int n)
{
    int *perm = snewn(n > 0 ? n : 1, int), i;
    unsigned char *pin = snewn(n > 0 ? n : 1, unsigned char);
    strbuf *sb = strbuf_new();
    ksp_order_apply(l, kind, names, n, perm, pin);
    for (i = 0; i < n; i++) {
        if (i)
            put_byte(sb, ' ');
        put_dataz(sb, names[perm[i]]);
        if (pin[perm[i]])
            put_byte(sb, '*');
    }
    sfree(pin);
    sfree(perm);
    return strbuf_to_str(sb);
}
static void expect_shown(const struct ksp_order_list *l, char kind,
                         char *const *names, int n, const char *want,
                         const char *what)
{
    char *got = shown(l, kind, names, n);
    char *msg = dupprintf("%s: \"%s\" (want \"%s\")", what, got, want);
    ok(!strcmp(got, want), msg);
    sfree(msg);
    sfree(got);
}

static char *const ABCDE[] = { "a", "b", "c", "d", "e" };

/* ------------------------------------------------------------------ */
static void part_resolver(void)
{
    struct ksp_order_list l;
    head("resolver");
    ksp_order_list_init(&l);
    expect_shown(&l, 's', ABCDE, 5, "a b c d e", "no places: the automatic order");

    ksp_order_list_add(&l, 's', "d", "a");
    ksp_order_list_add(&l, 's', "e", "d");
    expect_shown(&l, 's', ABCDE, 5, "a d* e* b c", "a chain after its anchor");
    expect_shown(&l, 'f', ABCDE, 5, "a b c d e", "the other kind is not touched");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "c", NULL);
    expect_shown(&l, 's', ABCDE, 5, "c* a b d e", "first");
    ksp_order_list_add(&l, 's', "e", NULL);
    expect_shown(&l, 's', ABCDE, 5, "c* e* a b d", "several first keep their stored sequence");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "b", "zz");
    expect_shown(&l, 's', ABCDE, 5, "a b c d e", "an anchor that is gone: the automatic place");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "a", "b");
    ksp_order_list_add(&l, 's', "b", "a");
    ksp_order_list_add(&l, 's', "e", "a");
    expect_shown(&l, 's', ABCDE, 5, "a e* b c d", "a cycle: its members stay, a follower still follows");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "c", "c");
    expect_shown(&l, 's', ABCDE, 5, "a b c d e", "anchored to itself: the automatic place");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "D", "A");
    expect_shown(&l, 's', ABCDE, 5, "a d* b c e", "names match without case");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "c", NULL);
    ksp_order_list_add(&l, 's', "a", "c");
    ksp_order_list_add(&l, 's', "e", "a");
    ksp_order_list_add(&l, 's', "b", "e");
    ksp_order_list_add(&l, 's', "d", "b");
    expect_shown(&l, 's', ABCDE, 5, "c* a* e* b* d*", "a level with only stored places");
    ksp_order_list_free(&l);

    ksp_order_list_add(&l, 's', "b", "e");
    ksp_order_list_add(&l, 's', "b", NULL);
    expect_shown(&l, 's', ABCDE, 5, "a c d e b*", "the first entry of a name counts");
    ksp_order_list_free(&l);

    {
        static char *const twins[] = { "Web", "web", "x", "y" };
        ksp_order_list_add(&l, 's', "WEB", "x");
        expect_shown(&l, 's', twins, 4, "x Web* web* y",
                     "two items of one shown name move together");
        ksp_order_list_free(&l);
    }
}

/* ------------------------------------------------------------------ */
/* The one comparator under every list (ksp_name_cmp, ksp_sort_shown):
 * natural and without case, then without case, then by byte; folders that
 * differ only in case are one folder. */
static void part_comparator(void)
{
    struct ksp_shown_row rows[5];
    static const char *const order1[] = { "linux\\b", "Linux\\a", "k8", "Linux\\c", "k78" };
    static const char *const order2[] = { "Linux\\c", "k78", "Linux\\a", "linux\\b", "k8" };
    char *a, *b;
    int i;
    head("the shared comparator");
    ok(ksp_name_cmp("k8", "k78") < 0, "digit runs by value: k8 before k78");
    ok(ksp_name_cmp("Web", "web") < 0 && ksp_name_cmp("web", "Web") > 0,
       "names equal without case: by byte, a total order");
    ok(ksp_name_cmp("web01", "WEB02") < 0, "case does not lead");
    /* the same input in two orders sorts the same, the case-only folders
     * side by side (one folder) */
    {
        strbuf *s1 = strbuf_new(), *s2 = strbuf_new();
        for (i = 0; i < 5; i++) {
            rows[i].id = order1[i];
            rows[i].folder = NULL;
            rows[i].idx = i;
        }
        for (i = 0; i < 5; i++) {
            char *f = ksp_folder_of(order1[i]);
            rows[i].folder = f;
        }
        ksp_sort_shown_by_folder(rows, 5);
        for (i = 0; i < 5; i++) {
            put_fmt(s1, "%s|", rows[i].id);
            sfree((char *)rows[i].folder);
        }
        for (i = 0; i < 5; i++) {
            rows[i].id = order2[i];
            rows[i].folder = ksp_folder_of(order2[i]);
            rows[i].idx = i;
        }
        ksp_sort_shown_by_folder(rows, 5);
        for (i = 0; i < 5; i++) {
            put_fmt(s2, "%s|", rows[i].id);
            sfree((char *)rows[i].folder);
        }
        a = strbuf_to_str(s1);
        b = strbuf_to_str(s2);
        ok(!strcmp(a, b), "the same rows in another input order sort the same");
        ok(!strcmp(a, "k8|k78|Linux\\a|linux\\b|Linux\\c|"),
           "Linux and linux one folder: its sessions together, by name");
        if (strcmp(a, "k8|k78|Linux\\a|linux\\b|Linux\\c|"))
            printf("      got: %s\n", a);
        sfree(a);
        sfree(b);
    }
}

/* ------------------------------------------------------------------ */
/* A plan applied to a list, and the level shown afterwards. */
static void apply_changes(struct ksp_order_list *l, const struct ksp_order_changes *ch)
{
    int i, j;
    for (i = 0; i < ch->n; i++) {
        for (j = 0; j < l->n; j++)
            if (l->e[j].kind == ch->c[i].kind && !stricmp(l->e[j].name, ch->c[i].name)) {
                sfree(l->e[j].name);
                sfree(l->e[j].anchor);
                memmove(&l->e[j], &l->e[j + 1], (l->n - j - 1) * sizeof(*l->e));
                l->n--;
                j--;
            }
        if (!ch->c[i].remove)
            ksp_order_list_add(l, ch->c[i].kind, ch->c[i].name, ch->c[i].anchor);
    }
}

static void part_planner(void)
{
    struct ksp_order_list l;
    struct ksp_order_changes ch;
    head("planner");
    ksp_order_list_init(&l);
    {
        static char *const want[] = { "a", "d", "b", "c", "e" };
        static char *const mv[] = { "d" };
        int n = ksp_order_plan(&l, 's', ABCDE, 5, want, 5, mv, 1, NULL, 0, &ch);
        ok(n == 1 && ch.c[0].moved && ch.c[0].anchor && !strcmp(ch.c[0].anchor, "a"),
           "a drop between a and b: d after a");
        apply_changes(&l, &ch);
        ksp_order_changes_free(&ch);
        expect_shown(&l, 's', ABCDE, 5, "a d* b c e", "and the list shows it");
    }
    {
        static char *const want[] = { "c", "a", "d", "b", "e" };
        static char *const mv[] = { "c" };
        ksp_order_plan(&l, 's', ABCDE, 5, want, 5, mv, 1, NULL, 0, &ch);
        apply_changes(&l, &ch);
        ksp_order_changes_free(&ch);
        expect_shown(&l, 's', ABCDE, 5, "c* a d* b e", "to the top, the other place kept");
    }
    {
        /* b dropped between a and d: d was anchored to a and follows b now */
        static char *const want[] = { "c", "a", "b", "d", "e" };
        static char *const mv[] = { "b" };
        ksp_order_plan(&l, 's', ABCDE, 5, want, 5, mv, 1, NULL, 0, &ch);
        apply_changes(&l, &ch);
        {
            const struct ksp_order_entry *e = ksp_order_find(&l, 's', "d");
            ok(e && e->anchor && !strcmp(e->anchor, "b"),
               "the item below the drop is anchored to the dropped one");
        }
        ksp_order_changes_free(&ch);
        expect_shown(&l, 's', ABCDE, 5, "c* a b* d* e", "exactly what the drop showed");
    }
    {
        static char *const want[] = { "c", "e", "a", "b", "d" };
        static char *const mv[] = { "e", "a", "b" };   /* a block of three */
        ksp_order_plan(&l, 's', ABCDE, 5, want, 5, mv, 3, NULL, 0, &ch);
        apply_changes(&l, &ch);
        ksp_order_changes_free(&ch);
        expect_shown(&l, 's', ABCDE, 5, "c* e* a* b* d*", "a block of three, each after the one before");
    }
    {
        static char *const rest[] = { "a", "c", "d", "e" };
        static char *const want[] = { "c", "e", "a", "d" };
        static char *const gone[] = { "b" };
        ksp_order_plan(&l, 's', rest, 4, want, 4, NULL, 0, gone, 1, &ch);
        apply_changes(&l, &ch);
        ksp_order_changes_free(&ch);
        ok(!ksp_order_find(&l, 's', "b"), "a session moved away loses its place");
        expect_shown(&l, 's', rest, 4, "c* e* a* d*", "and its follower stays where it was");
    }
    {
        static char *const rest[] = { "a", "c", "d", "e" };
        static char *const want[] = { "c", "e", "a", "d" };
        static char *const mv[] = { "e" };
        int n = ksp_order_plan(&l, 's', rest, 4, want, 4, mv, 1, NULL, 0, &ch);
        ok(n == 0, "a drop where it already is changes nothing");
        ksp_order_changes_free(&ch);
    }
    ksp_order_list_free(&l);
}

/* ------------------------------------------------------------------ */
static char *entries_text(const struct ksp_order_doc *d)
{
    struct ksp_order_list l;
    strbuf *sb = strbuf_new();
    int i;
    ksp_order_doc_entries(d, &l);
    for (i = 0; i < l.n; i++)
        put_fmt(sb, "%s%c:%s<%s", i ? " " : "", l.e[i].kind, l.e[i].name,
                l.e[i].anchor ? l.e[i].anchor : "^");
    ksp_order_list_free(&l);
    return strbuf_to_str(sb);
}

static void part_parser(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# sessions of this folder, in this order\r\n"
        "s\tweb01\r\n"
        "s\tweb02\r\n"
        "\r\n"
        "f\tdatabases\r\n"
        "x\tjunk\r\n"
        "s\t\r\n"
        "s web03\r\n"
        "s\tdb01\tweb01\r\n"
        "s\tweb03\r\n"
        "f\tlinux\tdatabases\textra";
    struct ksp_order_doc *d = ksp_order_doc_text(text, sizeof(text) - 1);
    char *e = entries_text(d);
    head(".kittyorder parser");
    ok(!strcmp(e, "s:web01<^ s:web02<web01 f:databases<^ s:db01<web01 "
                  "s:web03<db01 f:linux<databases"), e);
    ok(!ksp_order_doc_changed(d), "read and not changed: nothing to write");
    {
        size_t len;
        char *b = ksp_order_doc_bytes(d, &len);
        ok(len == sizeof(text) - 1 && !memcmp(b, text, len),
           "BOM, CRLF, comments, junk and the missing last line end kept byte for byte");
        sfree(b);
    }
    sfree(e);
    ksp_order_doc_free(d);

    d = ksp_order_doc_text("", 0);
    e = entries_text(d);
    ok(!*e && ksp_order_doc_empty(d), "no file: no places");
    sfree(e);
    ksp_order_doc_free(d);
}

/* ------------------------------------------------------------------ */
/* One edit: the level `names` (automatic order) shown as stored in
 * `before`, the block `mv` dropped below `above` (NULL = top); the file's
 * bytes afterwards against `after`. */
static void golden_drop(const char *what, const char *before,
                        char *const *names, int n, char *const *mv, int nmv,
                        const char *above, const char *after)
{
    struct ksp_order_doc *d = ksp_order_doc_text(before, strlen(before));
    struct ksp_order_list cur;
    struct ksp_order_changes ch;
    int *perm = snewn(n, int), i, j, k = 0;
    char **sh = snewn(n, char *), **want = snewn(n, char *);
    char *got, *msg;
    size_t len;

    ksp_order_doc_entries(d, &cur);
    ksp_order_apply(&cur, 's', names, n, perm, NULL);
    for (i = 0; i < n; i++)
        sh[i] = names[perm[i]];
    /* the desired order: the rows without the block, the block below `above` */
    if (!above)
        for (j = 0; j < nmv; j++)
            want[k++] = mv[j];
    for (i = 0; i < n; i++) {
        int inblock = 0;
        for (j = 0; j < nmv; j++)
            inblock |= !strcmp(sh[i], mv[j]);
        if (inblock)
            continue;
        want[k++] = sh[i];
        if (above && !strcmp(sh[i], above))
            for (j = 0; j < nmv; j++)
                want[k++] = mv[j];
    }
    ksp_order_plan(&cur, 's', names, n, want, k, mv, nmv, NULL, 0, &ch);
    ksp_order_doc_change(d, 's', names, n, &ch);
    got = ksp_order_doc_bytes(d, &len);
    msg = dupprintf("%s: [%s]", what, got);
    ok(!strcmp(got, after), msg);
    sfree(msg);
    /* the meaning: what the edited file shows is the order wanted */
    {
        struct ksp_order_list l2;
        struct ksp_order_doc *d2 = ksp_order_doc_text(got, len);
        bool same = true;
        ksp_order_doc_entries(d2, &l2);
        ksp_order_apply(&l2, 's', names, n, perm, NULL);
        for (i = 0; i < n && i < k; i++)
            same = same && !strcmp(names[perm[i]], want[i]);
        msg = dupprintf("%s: the file shows the order of the drop", what);
        ok(same, msg);
        sfree(msg);
        ksp_order_list_free(&l2);
        ksp_order_doc_free(d2);
    }
    sfree(got);
    ksp_order_changes_free(&ch);
    ksp_order_list_free(&cur);
    sfree(want);
    sfree(sh);
    sfree(perm);
    ksp_order_doc_free(d);
}

static void part_edit(void)
{
    static char *const W3[] = { "web01", "web02", "web03" };
    static char *const W4[] = { "web01", "web02", "web03", "web04" };
    head(".kittyorder edited in place");
    {
        static char *const mv[] = { "web03" };
        golden_drop("to the top (a new line, the rest untouched)",
                    "# c\ns\tweb01\ns\tweb02\nf\tdatabases\n", W3, 3, mv, 1, NULL,
                    "# c\ns\tweb03\ns\tweb01\ns\tweb02\nf\tdatabases\n");
    }
    {
        static char *const mv[] = { "web01" };
        golden_drop("move down (one line moves)",
                    "s\tweb01\ns\tweb02\ns\tweb03\n", W4, 4, mv, 1, "web02",
                    "s\tweb02\ns\tweb01\ns\tweb03\n");
    }
    {
        static char *const mv[] = { "web03" };
        golden_drop("move up, CRLF and BOM kept",
                    "\xEF\xBB\xBF# keep\r\ns\tweb01\r\ns\tweb02\r\ns\tweb03\r\n", W3, 3, mv, 1,
                    "web01",
                    "\xEF\xBB\xBF# keep\r\ns\tweb01\r\ns\tweb03\r\ns\tweb02\r\n");
    }
    {
        static char *const mv[] = { "web04" };
        golden_drop("onto an item with no place (anchor written out)",
                    "s\tweb01\n", W4, 4, mv, 1, "web02",
                    "s\tweb01\ns\tweb04\tweb02\n");
    }
    {
        static char *const mv[] = { "web02" };
        golden_drop("a line of a name not in the folder stays byte for byte",
                    "s\tweb01\ns\tghost\tweb01\ns\tweb03\tweb01\n", W3, 3, mv, 1, "web03",
                    "s\tweb01\ns\tghost\tweb01\ns\tweb03\tweb01\ns\tweb02\n");
    }
    {
        static char *const mv[] = { "web02" };
        golden_drop("a drop where it already is writes nothing",
                    "# x\ns\tweb01\ns\tweb02\n", W3, 3, mv, 1, "web01",
                    "# x\ns\tweb01\ns\tweb02\n");
    }
    {
        static const char same[] = "# x\ns\tweb01\ns\tweb02\n";
        struct ksp_order_doc *d = ksp_order_doc_text(same, sizeof(same) - 1);
        ok(!ksp_order_doc_changed(d), "and the document reports no change");
        ksp_order_doc_free(d);
    }
    {
        static const char before[] = "s\tweb01\ns\tweb02\ns\tdb\tweb02\n";
        struct ksp_order_doc *d = ksp_order_doc_text(before, sizeof(before) - 1);
        size_t len;
        char *b;
        ksp_order_doc_rename(d, 's', "WEB02", "web2");
        b = ksp_order_doc_bytes(d, &len);
        ok(!strcmp(b, "s\tweb01\ns\tweb2\ns\tdb\tweb2\n"),
           "rename: the name field and the anchors that carry it");
        sfree(b);
        ksp_order_doc_free(d);
    }
    {
        /* a session moved away: its line goes, the next two-field line now
         * follows the line before, which is its anchor's anchor */
        static const char before[] = "s\tweb01\ns\tweb02\ns\tweb03\n";
        struct ksp_order_doc *d = ksp_order_doc_text(before, sizeof(before) - 1);
        struct ksp_order_changes ch;
        size_t len;
        char *b;
        memset(&ch, 0, sizeof(ch));
        {
            static char *const rest[] = { "web01", "web03" };
            static char *const gone[] = { "web02" };
            struct ksp_order_list cur;
            ksp_order_doc_entries(d, &cur);
            ksp_order_plan(&cur, 's', rest, 2, rest, 2, NULL, 0, gone, 1, &ch);
            ksp_order_doc_change(d, 's', rest, 2, &ch);
            ksp_order_list_free(&cur);
        }
        b = ksp_order_doc_bytes(d, &len);
        ok(!strcmp(b, "s\tweb01\ns\tweb03\n"), "forget: one line deleted, nothing else");
        sfree(b);
        ksp_order_changes_free(&ch);
        ksp_order_doc_free(d);
    }
    {
        static const char before[] = "# generated\ns\ta\nf\tb\n\n";
        struct ksp_order_doc *d = ksp_order_doc_text(before, sizeof(before) - 1);
        size_t len;
        char *b;
        ksp_order_doc_reset(d);
        b = ksp_order_doc_bytes(d, &len);
        ok(!strcmp(b, "# generated\n\n") && !ksp_order_doc_empty(d),
           "Reset Order: the item lines go, the comment stays (the file stays)");
        sfree(b);
        ksp_order_doc_free(d);
        d = ksp_order_doc_text("s\ta\nf\tb\n", 8);
        ksp_order_doc_reset(d);
        ok(ksp_order_doc_empty(d) && ksp_order_doc_changed(d),
           "Reset Order on a file of items only: empty (the file is deleted)");
        ksp_order_doc_free(d);
    }
}

/* ------------------------------------------------------------------ */
static char *g_dir;

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    strbuf *sb = strbuf_new();
    int c;
    if (!f) {
        strbuf_free(sb);
        return NULL;
    }
    while ((c = fgetc(f)) != EOF)
        put_byte(sb, (unsigned char)c);
    fclose(f);
    return strbuf_to_str(sb);
}

static void part_folder_store(void)
{
    struct ksp_order_store st;
    char *p, *t;
    const struct ksp_order_list *l;
    static char *const names[] = { "web01", "web02", "web03" };
    head("folder store");
    memset(&st, 0, sizeof(st));
    st.folder_store = 1;
    st.dir = dupstr(g_dir);

    p = ksp_order_location(&st, "Linux\\web");
    t = dupprintf("%s\\Linux\\web\\.kittyorder", g_dir);
    ok(!strcmp(p, t), "a folder's file: <dir>\\<folder>\\.kittyorder");
    sfree(t);
    sfree(p);
    p = ksp_order_location(&st, "Default");
    t = dupprintf("%s\\.kittyorder", g_dir);
    ok(!strcmp(p, t), "the root's file is in the session directory");
    sfree(t);
    sfree(p);

    /* written by hand, as a tool would */
    t = dupprintf("mkdir \"%s\\Linux\\web\" 2>nul", g_dir);
    system(t);
    sfree(t);
    p = ksp_order_location(&st, "Linux\\web");
    write_text(p, "# tool\ns\tweb03\ns\tweb01\n");
    l = ksp_order_load(&st, "Linux\\web");
    expect_shown(l, 's', names, 3, "web03* web01* web02", "a file written by a tool is read");
    l = ksp_order_load(&st, "Linux\\web");
    ok(l && l->n == 2, "the second load comes from the cache");
    write_text(p, "s\tweb02\n");
    l = ksp_order_load(&st, "Linux\\web");
    expect_shown(l, 's', names, 3, "web02* web01 web03",
                 "a changed file is read again (its size and time)");
    write_text(p, "\x01\x02garbage\nnot a line\n\n");
    l = ksp_order_load(&st, "Linux\\web");
    expect_shown(l, 's', names, 3, "web01 web02 web03", "a damaged file: the automatic order");

    /* written by the store: in place */
    write_text(p, "# tool\ns\tweb03\ns\tweb01\n");
    {
        struct ksp_order_doc *d = ksp_order_open(&st, "Linux\\web");
        struct ksp_order_list cur;
        struct ksp_order_changes ch;
        static char *const want[] = { "web03", "web02", "web01" };
        static char *const mv[] = { "web02" };
        ksp_order_doc_entries(d, &cur);
        ksp_order_plan(&cur, 's', names, 3, want, 3, mv, 1, NULL, 0, &ch);
        ksp_order_doc_change(d, 's', names, 3, &ch);
        ok(ksp_order_commit(&st, "Linux\\web", d), "committed");
        ksp_order_changes_free(&ch);
        ksp_order_list_free(&cur);
        ksp_order_doc_free(d);
        t = read_text(p);
        ok(t && !strcmp(t, "# tool\ns\tweb03\ns\tweb02\ns\tweb01\n"),
           "the drop edits the tool's file: one line added");
        sfree(t);
    }

    /* the folder renamed: the file follows the path */
    ok(ksp_order_folder_moved(&st, "Linux\\web", "Linux\\www", 0), "folder moved");
    t = dupprintf("%s\\Linux\\www\\.kittyorder", g_dir);
    ok(GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES &&
       GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES,
       "its .kittyorder moved with it, the old folder pruned");
    sfree(t);
    sfree(p);
    l = ksp_order_load(&st, "Linux\\www");
    expect_shown(l, 's', names, 3, "web03* web02* web01*", "and is read at the new path");

    /* the parent's entry of the folder follows the rename */
    p = ksp_order_location(&st, "Linux");
    write_text(p, "f\tzz\nf\twww\n");
    ok(ksp_order_rename(&st, "Linux", 'f', "www", "w3"), "renamed in the parent");
    t = read_text(p);
    ok(t && !strcmp(t, "f\tzz\nf\tw3\n"), "the parent's line carries the new name");
    sfree(t);
    ok(ksp_order_forget(&st, "Linux", 'f', "zz"), "forgotten");
    t = read_text(p);
    ok(t && !strcmp(t, "f\tw3\n"), "its line deleted; the follower now first, as zz was");
    sfree(t);
    ok(ksp_order_reset(&st, "Linux"), "Reset Order");
    ok(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, "a file of items only is deleted");
    sfree(p);

    /* deleted with everything in it */
    ok(ksp_order_folder_moved(&st, "Linux\\www", NULL, 0), "folder deleted");
    t = dupprintf("%s\\Linux\\www\\.kittyorder", g_dir);
    ok(GetFileAttributesA(t) == INVALID_FILE_ATTRIBUTES, "its places are gone");
    sfree(t);
    ksp_order_store_free(&st);
}

/* ------------------------------------------------------------------ */
typedef LONG (WINAPI *RegLoadAppKeyA_t)(LPCSTR, PHKEY, REGSAM, DWORD, DWORD);

static bool reg_exists(HKEY root, const char *key)
{
    HKEY k;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

static void part_registry(const char *tmp)
{
    RegLoadAppKeyA_t load = (RegLoadAppKeyA_t)(void *)GetProcAddress(
        GetModuleHandleA("advapi32.dll"), "RegLoadAppKeyA");
    struct ksp_order_store st;
    char *hive, *cmd, *loc;
    HKEY root;
    const struct ksp_order_list *l;
    static char *const names[] = { "web01", "web02", "web03" };

    head("registry store (private hive)");
    if (!load) {
        printf("  SKIP  RegLoadAppKey not available\n");
        return;
    }
    hive = dupprintf("%s\\kitty_test_sessorder.hiv", tmp);
    cmd = dupprintf("del /q \"%s*\" 2>nul", hive);
    system(cmd);
    if (load(hive, &root, KEY_ALL_ACCESS, 0, 0) != ERROR_SUCCESS) {
        ok(false, "a private hive file can be loaded");
        sfree(hive);
        sfree(cmd);
        return;
    }
    memset(&st, 0, sizeof(st));
    st.regbase = dupstr("Base");
    st.regroot = root;

    loc = ksp_order_location(&st, NULL);
    ok(!strcmp(loc, "Base\\SessionOrder\\%5C"), "the root's key is %5C");
    sfree(loc);
    loc = ksp_order_location(&st, "Linux\\web");
    ok(!strcmp(loc, "Base\\SessionOrder\\Linux%5Cweb"), "a folder escaped as Sessions escapes it");
    sfree(loc);

    {
        struct ksp_order_doc *d = ksp_order_open(&st, "Linux\\web");
        struct ksp_order_list cur;
        struct ksp_order_changes ch;
        static char *const want[] = { "web03", "web01", "web02" };
        static char *const mv[] = { "web03" };
        ksp_order_doc_entries(d, &cur);
        ksp_order_plan(&cur, 's', names, 3, want, 3, mv, 1, NULL, 0, &ch);
        ksp_order_doc_change(d, 's', names, 3, &ch);
        ok(ksp_order_commit(&st, "Linux\\web", d), "committed to the key");
        ksp_order_changes_free(&ch);
        ksp_order_list_free(&cur);
        ksp_order_doc_free(d);
    }
    ok(reg_exists(root, "Base\\SessionOrder\\Linux%5Cweb"), "the key exists");
    l = ksp_order_load(&st, "Linux\\web");
    expect_shown(l, 's', names, 3, "web03* web01 web02", "read back from the values");
    l = ksp_order_load(&st, "LINUX\\WEB");
    expect_shown(l, 's', names, 3, "web03* web01 web02", "a folder keyed without case");

    ok(ksp_order_folder_moved(&st, "Linux", "Unix", 0), "a folder moved");
    ok(!reg_exists(root, "Base\\SessionOrder\\Linux%5Cweb") &&
       reg_exists(root, "Base\\SessionOrder\\Unix%5Cweb"),
       "the keys below it follow the path");
    ok(ksp_order_reset(&st, "Unix\\web"), "Reset Order");
    ok(!reg_exists(root, "Base\\SessionOrder\\Unix%5Cweb"), "the key with no value is removed");

    ksp_order_cache_clear();
    sfree(st.regbase);
    RegCloseKey(root);
    system(cmd);
    sfree(hive);
    sfree(cmd);
}

int main(void)
{
    const char *tmp = getenv("TEMP");
    char *cmd;
    if (!tmp || !*tmp)
        tmp = getenv("TMP");
    if (!tmp || !*tmp)
        tmp = ".";
    g_dir = dupprintf("%s\\kitty_test_sessorder", tmp);
    /* no kitty.ini in charge: readonly=yes of the machine's own stays out */
    cmd = dupprintf("%s\\none\\kitty.ini", g_dir);
    kitty_inifile_pin(cmd);
    sfree(cmd);

    part_comparator();
    part_resolver();
    part_planner();
    part_parser();
    part_edit();

    cmd = dupprintf("rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);
    sfree(cmd);
    cmd = dupprintf("mkdir \"%s\" 2>nul", g_dir);
    system(cmd);
    sfree(cmd);
    part_folder_store();
    part_registry(tmp);
    ksp_order_cache_clear();
    cmd = dupprintf("rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);
    sfree(cmd);
    sfree(g_dir);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
