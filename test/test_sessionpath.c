/*
 * test_sessionpath.c - session names as folder paths (hknet/KiTTY#55) and the
 * session file suffix (hknet/KiTTY#56).
 *
 * Part one drives the pure helpers in kitty/kitty_sessionpath.c: the escape
 * of one path component (reserved device names, a leading dot, a trailing dot
 * or space), path <-> registry key, the name lookup, the suffix strip and the
 * "is this file a session" check.
 *
 * Part two runs the folder store against a temp directory the test creates
 * and removes (kitty_set_storage_mode(1) + kitty_set_session_dir), never the
 * registry and never a real session: nested layout, the same name in two
 * folders, the suffix and its legacy files, the save clash, dot-files and
 * non-session files, and the resolver behind -load.
 *
 * Known limit, by design of the content check: a text file with a line
 * starting "HostName=" counts as a session. Asserted below so it stays a
 * known limit rather than a surprise.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "kitty/kitty_storage.h"
#include "kitty/kitty_sessionpath.h"
#include "kitty/kitty_sessionrekey.h"

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

/* kitty_sessionrekey.c's GUI-side neighbours, for the fixture store only:
 * the per-session commands' file name (the folder store's escape is enough
 * here) and "does this session exist" (kitty_migrate.c's rule: a FILE). */
void mungestr(const char *in, char *out)
{
    char *m = ksf_munge(in);
    strcpy(out, m);
    sfree(m);
}
bool kitty_own_session_exists(const char *name)
{
    char *p = (name && *name) ? ksf_session_find(name) : NULL;
    bool yes = p != NULL;
    sfree(p);
    return yes;
}

void add_session_to_jumplist(const char * const sessionname) {}
void remove_session_from_jumplist(const char * const sessionname) {}
void clear_jumplist(void) {}

HandleWait *add_handle_wait(HANDLE h, handle_wait_callback_fn_t cb, void *ctx)
{ unreachable("no event loop in this test"); }
void delete_handle_wait(HandleWait *hw)
{ unreachable("no event loop in this test"); }

void old_keyfile_warning(void) {}
const bool share_can_be_upstream = false;
const bool share_can_be_downstream = false;

char *platform_default_s(const char *name) { return NULL; }
bool platform_default_b(const char *name, bool def) { return def; }
int platform_default_i(const char *name, int def) { return def; }
FontSpec *platform_default_fontspec(const char *name)
{ return fontspec_new_default(); }
Filename *platform_default_filename(const char *name)
{ return filename_from_str(""); }

static int failures = 0, checks = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

static void ok_eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got && want && !strcmp(got, want)) {
        printf("  PASS  %s\n", what);
    } else if (!got && !want) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s (got \"%s\", wanted \"%s\")\n",
               what, got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void head(const char *s) { printf("\n=== %s ===\n", s); }

/* One component: escape, check the expected file name, and back again. */
static void comp_round_trip(const char *name, const char *want_file)
{
    char *m = ksp_component_munge(name);
    char *u = ksp_component_unmunge(m);
    char what[256];
    if (want_file) {
        snprintf(what, sizeof(what), "\"%s\" is stored as \"%s\"", name, want_file);
        ok_eq_str(m, want_file, what);
    }
    snprintf(what, sizeof(what), "\"%s\" reads back unchanged", name);
    ok_eq_str(u, name, what);
    sfree(m);
    sfree(u);
}

static void part_pure(void)
{
    head("one path component: escapes round-trip");
    comp_round_trip("srv01", "srv01");
    comp_round_trip("a:b", "a%3Ab");
    comp_round_trip("a/b*c?d\"e<f>g|h%i", "a%2Fb%2Ac%3Fd%22e%3Cf%3Eg%7Ch%25i");
    comp_round_trip("tab\there", "tab%09here");
    comp_round_trip("name.", "name%2E");
    comp_round_trip("name ", "name%20");
    comp_round_trip(".hidden", "%2Ehidden");
    comp_round_trip("..", "%2E%2E");
    comp_round_trip("a b.c-d_e", "a b.c-d_e");

    head("reserved device names, with and without an extension");
    comp_round_trip("con", "%63on");
    comp_round_trip("CON", "%43ON");
    comp_round_trip("CON.txt", "%43ON.txt");
    comp_round_trip("COM1", "%43OM1");
    comp_round_trip("LPT9", "%4CPT9");
    comp_round_trip("nul.tar.gz", "%6Eul.tar.gz");
    comp_round_trip("aux ", NULL);
    comp_round_trip("CONSOLE", "CONSOLE");
    comp_round_trip("COM10", "COM10");
    comp_round_trip("LPT0", "LPT0");

    head("a path: every component on its own");
    {
        char *rel = ksp_path_to_relfile("Linux\\web\\srv01", "");
        ok_eq_str(rel, "Linux\\web\\srv01", "plain path -> nested relative file");
        sfree(rel);
        rel = ksp_path_to_relfile("CON\\a:b\\LPT9.", ".ktx");
        ok_eq_str(rel, "%43ON\\a%3Ab\\%4CPT9%2E.ktx",
                  "reserved name and escapes per directory component, suffix last");
        sfree(rel);
        rel = ksp_path_to_relfile("\\x\\\\y\\", "");
        ok_eq_str(rel, "x\\y", "empty components are dropped");
        sfree(rel);
    }

    head("a session file where another session needs a folder");
    {
        const char *a[] = { "Linux\\web\\srv01", "foo\\bar", "zzz", "FOO" };
        const char *b[] = { "foo\\bar", "foo bar", "foo-x", "foo2\\x" };
        char *blk = ksp_layout_blocker(a, 4, "");
        ok_eq_str(blk, "FOO", "foo\\bar needs the folder that FOO's file is (any case)");
        sfree(blk);
        blk = ksp_layout_blocker(a, 4, ".ktx");
        ok(blk == NULL, "a suffix keeps file and folder apart");
        sfree(blk);
        blk = ksp_layout_blocker(b, 4, "");
        ok(blk == NULL, "names sorting between foo and foo\\bar are no clash");
        sfree(blk);
        blk = ksp_layout_blocker(a, 0, "");
        ok(blk == NULL, "no sessions, no clash");
        sfree(blk);
    }

    head("folder and leaf of a path");
    {
        char *f = ksp_folder_of("Linux\\web\\srv01");
        ok_eq_str(f, "Linux\\web", "folder of Linux\\web\\srv01");
        sfree(f);
        f = ksp_folder_of("srv01");
        ok(f == NULL, "a root session has no folder");
        ok_eq_str(ksp_leaf("Linux\\web\\srv01"), "srv01", "leaf of a path");
        ok_eq_str(ksp_leaf("srv01"), "srv01", "leaf of a root name");
    }

    head("folders as paths");
    {
        char *t;
        ok(ksp_folder_same("Linux", "linux") && ksp_folder_same("", "Default") &&
           !ksp_folder_same("Linux", "Default"), "folder compare: case-insensitive, root spellings");
        ok(ksp_folder_within("a\\b", "a") && ksp_folder_within("a", "a") &&
           !ksp_folder_within("ab", "a") && ksp_folder_within("x", "Default"),
           "within: the folder and its subtree, not a name that merely starts alike");
        t = ksp_folder_child_row("Default", "network\\test\\deep");
        ok_eq_str(t, "network", "a root row shows the top level only");
        sfree(t);
        t = ksp_folder_child_row("network", "network\\test\\deep");
        ok_eq_str(t, "network\\test", "inside network its subfolder is a row");
        sfree(t);
        t = ksp_folder_child_row("network", "other\\x");
        ok(t == NULL, "a folder elsewhere is no row here");
        sfree(t);
        t = ksp_folder_parent("network\\test");
        ok_eq_str(t, "network", ".. of network\\test is network");
        sfree(t);
        t = ksp_folder_parent("network");
        ok_eq_str(t, "Default", ".. of a top-level folder is the root");
        sfree(t);
        t = ksp_folder_moved_path("a\\b\\srv", "a", "Default");
        ok_eq_str(t, "b\\srv", "moving a to the root keeps b\\srv below it");
        sfree(t);
        t = ksp_folder_moved_path("a\\b\\srv", "a", "x\\y");
        ok_eq_str(t, "x\\y\\b\\srv", "moving a into x\\y keeps the subtree's shape");
        sfree(t);
    }

    head("the order a list shows sessions in");
    {
        /* One folder, both storage forms: a bare name filed by Folder=VSS and
         * four "VSS\..." paths. Their identities sort monitor first; their
         * shown names do not. */
        struct ksp_shown_row r[6] = {
            { "VSS\\k79.example-root", NULL, 0 },
            { "monitor.example", "VSS", 1 },
            { "VSS\\k8.example-root", NULL, 2 },
            { "Default Settings", NULL, 3 },
            { "VSS\\k78.example-root", NULL, 4 },
            { "VSS\\k7.example-root", NULL, 5 },
        };
        int i, want[6] = { 3, 5, 2, 4, 0, 1 }, same = 1;
        for (i = 0; i < 6; i++)
            if (!r[i].folder && strchr(r[i].id, '\\'))
                r[i].folder = "VSS";
        ksp_sort_shown(r, 6);
        for (i = 0; i < 6; i++)
            same &= (r[i].idx == want[i]);
        ok(same, "Default Settings, k7, k8, k78, k79, monitor - by the shown "
           "name, whatever form each is stored in");
        ok(ksp_natcasecmp("k8", "k78") < 0 && ksp_natcasecmp("host2", "Host10") < 0,
           "digit runs by value, case ignored");
        ok(ksp_natcasecmp("Monitor", "monitor") == 0 &&
           ksp_natcasecmp("k007", "k7") == 0, "case and zero padding alone are equal");
        ok(ksp_natcasecmp("net\\x", "net-2") < 0 && ksp_natcasecmp("net", "net\\x") < 0,
           "a folder's subfolders follow it, ahead of a sibling that starts alike");
    }
    {
        /* classic root list: "srv01 [Linux]" - the name leads, then the folder */
        struct ksp_shown_row r[3] = {
            { "Linux\\srv01", "Linux", 0 },
            { "srv01", NULL, 1 },
            { "alpha", "Zeta", 2 },
        };
        ksp_sort_shown(r, 3);
        ok(r[0].idx == 2 && r[1].idx == 1 && r[2].idx == 0,
           "by name first, the root before a folder for the same name");
        ksp_sort_shown_by_folder(r, 3);
        ok(r[0].idx == 1 && r[1].idx == 0 && r[2].idx == 2,
           "grouped by folder: the root, Linux, Zeta");
    }

    head("path <-> registry key (PuTTY's escape, one flat key)");
    {
        strbuf *k = strbuf_new(), *back = strbuf_new();
        escape_registry_key("Linux\\web\\srv01", k);
        ok_eq_str(k->s, "Linux%5Cweb%5Csrv01", "path -> Linux%5Cweb%5Csrv01");
        unescape_registry_key(k->s, back);
        ok_eq_str(back->s, "Linux\\web\\srv01", "and back to the path");
        strbuf_free(k);
        strbuf_free(back);
    }

    head("suffix strip");
    {
        char b[64];
        strcpy(b, "foo.ktx");
        ok(ksp_strip_suffix(b, ".ktx") && !strcmp(b, "foo"), "foo.ktx -> foo");
        strcpy(b, "foo.KTX");
        ok(ksp_strip_suffix(b, ".ktx") && !strcmp(b, "foo"), "case-insensitive");
        strcpy(b, "foo");
        ok(!ksp_strip_suffix(b, ".ktx") && !strcmp(b, "foo"), "no suffix: unchanged");
        strcpy(b, ".ktx");
        ok(!ksp_strip_suffix(b, ".ktx"), "nothing left in front: not stripped");
        strcpy(b, "fooktx");
        ok(!ksp_strip_suffix(b, ".ktx"), "a name merely ending like it is not stripped");
        strcpy(b, "foo.ktx");
        ok(!ksp_strip_suffix(b, ""), "empty suffix strips nothing");
    }

    head("what counts as a session file");
    {
        const char *kv = "Folder=Linux\nHostName=srv01\n";
        const char *cl = "Present\\1\\\r\nProtocol\\ssh\\\r\n";
        const char *ho = "HostName=\n";
        const char *po = "Protocol=ssh\n";
        const char *readme = "# Sessions\n\nHostName of each server goes here.\n";
        const char *gi = "*.log\n.DS_Store\n";
        const char *log = "[debug] HostName lookup failed\nProtocol error\n";
        const char *limit = "notes\nHostName=not really a session\n";
        ok(ksp_text_is_session(kv, strlen(kv)), "Key=value form");
        ok(ksp_text_is_session(cl, strlen(cl)), "Key\\value\\ form (classic, .ktx)");
        ok(ksp_text_is_session(ho, strlen(ho)), "HostName only (even empty)");
        ok(ksp_text_is_session(po, strlen(po)), "Protocol only");
        ok(!ksp_text_is_session(readme, strlen(readme)), "README.md is not a session");
        ok(!ksp_text_is_session(gi, strlen(gi)), ".gitignore is not a session");
        ok(!ksp_text_is_session(log, strlen(log)), "debug.log is not a session");
        ok(ksp_text_is_session(limit, strlen(limit)),
           "KNOWN LIMIT: a text file with a line starting HostName= counts");
    }

    head("name lookup");
    {
        char *names[] = { "Default Settings", "srv01", "Linux\\web\\srv02",
                          "Linux\\web\\srv03", "BSD\\srv03", "Linux\\srv04" };
        int n = lenof(names), m[8], nm;
        ok(ksp_lookup(names, n, "Linux\\web\\srv02", m, 8, &nm) == KSP_EXACT &&
           m[0] == 2, "path exact");
        ok(ksp_lookup(names, n, "linux\\WEB\\srv02", m, 8, &nm) == KSP_EXACT &&
           m[0] == 2, "path exact, case-insensitive");
        ok(ksp_lookup(names, n, "srv01", m, 8, &nm) == KSP_EXACT && m[0] == 1,
           "a root session by its bare name");
        ok(ksp_lookup(names, n, "srv02", m, 8, &nm) == KSP_UNIQUE && m[0] == 2,
           "bare unique -> that session");
        ok(ksp_lookup(names, n, "SRV04", m, 8, &nm) == KSP_UNIQUE && m[0] == 5,
           "bare unique, case-insensitive");
        ok(ksp_lookup(names, n, "srv03", m, 8, &nm) == KSP_AMBIGUOUS && nm == 2 &&
           m[0] == 3 && m[1] == 4, "bare ambiguous with both matches");
        ok(ksp_lookup(names, n, "nosuch", m, 8, &nm) == KSP_NONE, "unknown name");
        ok(ksp_lookup(names, n, "web\\srv02", m, 8, &nm) == KSP_NONE,
           "a partial path is never guessed");
        {
            char *list[] = { "Linux\\web\\srv03", "BSD\\srv03" };
            char *t = ksp_ambiguous_text("srv03", list, 2, 1);
            ok_eq_str(t, "Session \"srv03\" exists in more than one folder:\n"
                      "Linux\\web\\srv03\nBSD\\srv03\n\n"
                      "Load it by its full path, for example -load "
                      "\"Linux\\web\\srv03\".", "the -load text names the matches");
            sfree(t);
        }
    }
}

/* ------------------------------------------------------------------ */

static char g_dir[1024];

static void make_session(const char *name, const char *host, const char *folder)
{
    char *errmsg = NULL;
    settings_w *w = open_settings_w(name, &errmsg);
    if (!w) {
        printf("  FAIL  could not create fixture session \"%s\": %s\n",
               name, errmsg ? errmsg : "(no message)");
        sfree(errmsg);
        failures++;
        return;
    }
    write_setting_s(w, "HostName", host);
    if (folder)
        write_setting_s(w, "Folder", folder);
    close_settings_w(w);
}

static char *read_host(const char *name)
{
    settings_r *r = open_settings_r(name);
    char *h = r ? read_setting_s(r, "HostName") : NULL;
    if (r)
        close_settings_r(r);
    return h;
}

static bool file_exists(const char *rel)
{
    char *p = dupprintf("%s\\%s", g_dir, rel);
    DWORD a = GetFileAttributesA(p);
    sfree(p);
    return a != INVALID_FILE_ATTRIBUTES;
}

static void write_file(const char *rel, const char *text)
{
    char *p = dupprintf("%s\\%s", g_dir, rel);
    FILE *fp;
    ksf_make_parent_dirs(p);
    fp = fopen(p, "wb");
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
    sfree(p);
}

static bool listed(const char *name)
{
    int n, i;
    bool yes = false;
    char **names = kitty_session_names(&n);
    for (i = 0; i < n; i++)
        if (!strcmp(names[i], name))
            yes = true;
    kitty_session_names_free(names, n);
    return yes;
}

static void part_store(void)
{
    head("folder store: nested layout");
    make_session("Linux\\web\\srv01", "web1.example", "Linux\\web");
    ok(file_exists("Linux\\web\\srv01"), "Linux\\web\\srv01 is Sessions\\Linux\\web\\srv01");
    ok(listed("Linux\\web\\srv01"), "and is listed under its path");
    {
        char *h = read_host("Linux\\web\\srv01");
        ok_eq_str(h, "web1.example", "and loads by its path");
        sfree(h);
    }

    head("the same name in two folders");
    make_session("BSD\\srv01", "bsd1.example", "BSD");
    {
        char *a = read_host("Linux\\web\\srv01"), *b = read_host("BSD\\srv01");
        ok_eq_str(a, "web1.example", "Linux\\web\\srv01 keeps its settings");
        ok_eq_str(b, "bsd1.example", "BSD\\srv01 has its own");
        sfree(a);
        sfree(b);
    }

    head("escaped directory names and reserved names");
    make_session("a:b\\CON", "con.example", "a:b");
    ok(file_exists("a%3Ab\\%43ON"), "a:b\\CON is stored as a%3Ab\\%43ON");
    ok(listed("a:b\\CON"), "and lists back as a:b\\CON");

    head("the folder: the path wins over Folder=");
    make_session("Linux\\srv04", "s4.example", "Wrong");
    {
        char *f = kitty_read_session_folder("Linux\\srv04");
        ok_eq_str(f, "Linux", "a path session is in its path's folder");
        sfree(f);
    }
    make_session("unarranged", "u.example", "Linux\\web");
    {
        char *f = kitty_read_session_folder("unarranged");
        ok_eq_str(f, "Linux\\web", "a bare session still reports its Folder value");
        sfree(f);
    }

    head("dot-files and files that are not sessions");
    write_file(".gitignore", "*.log\n");
    write_file("README.md", "# my sessions\n");
    write_file("debug.log", "connect failed\n");
    write_file(".git\\config", "HostName=not a session\n");
    write_file("plain", "HostName=plain.example\n");
    ok(!listed(".gitignore"), ".gitignore is not listed");
    ok(!listed("README.md"), "README.md is not listed");
    ok(!listed("debug.log"), "debug.log is not listed");
    ok(!listed(".git\\config"), "nothing inside .git is listed");
    ok(listed("plain"), "a plain session file is listed");

    head("delete removes the file and an emptied folder");
    make_session("tmp\\deep\\gone", "g.example", NULL);
    del_settings("tmp\\deep\\gone");
    ok(!file_exists("tmp\\deep\\gone"), "the file is gone");
    ok(!file_exists("tmp"), "and so are its emptied folders");

    head("resolver behind -load");
    {
        char *res = NULL, *err = NULL;
        int k = kitty_session_resolve("Linux\\web\\srv01", &res, &err, 1);
        ok(k == KSP_EXACT && res && !strcmp(res, "Linux\\web\\srv01"), "path exact");
        sfree(res); sfree(err);
        k = kitty_session_resolve("srv04", &res, &err, 1);
        ok(k == KSP_UNIQUE && res && !strcmp(res, "Linux\\srv04"), "bare unique");
        sfree(res); sfree(err);
        k = kitty_session_resolve("srv01", &res, &err, 1);
        ok(k == KSP_AMBIGUOUS && !res && err &&
           strstr(err, "Linux\\web\\srv01") && strstr(err, "BSD\\srv01"),
           "bare ambiguous names both matches");
        sfree(res); sfree(err);
        k = kitty_session_resolve("unarranged", &res, &err, 1);
        ok(k == KSP_EXACT, "an unarranged session keeps working by bare name");
        sfree(res); sfree(err);
    }

    head("session file suffix");
    kitty_set_session_suffix(".ktx");
    ok(listed("plain"), "a legacy file without the suffix is still listed");
    make_session("withsuffix", "ws.example", NULL);
    ok(file_exists("withsuffix.ktx"), "a new save writes withsuffix.ktx");
    ok(listed("withsuffix"), "and lists as withsuffix");
    write_file("junk.ktx", "a note, named like a session file\n");
    ok(!listed("junk"), "a file with the suffix but no HostName/Protocol is not listed");
    write_file("byhand.ktx", "Protocol=ssh\n");
    ok(listed("byhand"), "a file with the suffix and a session key is listed");
    make_session("plain", "plain2.example", NULL);
    ok(file_exists("plain.ktx") && !file_exists("plain"),
       "a legacy file gets the suffix on its next save");
    write_file("dup", "HostName=old.example\n");
    write_file("dup.ktx", "HostName=new.example\n");
    {
        char *errmsg = NULL;
        settings_w *w = open_settings_w("dup", &errmsg);
        ok(w == NULL && errmsg && strstr(errmsg, "dup.ktx"),
           "save clash: refused, naming the existing file");
        if (w)
            close_settings_w(w);
        sfree(errmsg);
    }
    write_file("dup.kts", "HostName=other.example\n");
    {
        strbuf *cl = strbuf_new();
        int n = kitty_session_suffix_rename(".ktx", ".kts", 1, cl);
        ok(n >= 2 && strstr(cl->s, "dup.ktx") != NULL,
           "suffix change (dry run) counts the files and names the clash");
        strbuf_clear(cl);
        n = kitty_session_suffix_rename(".ktx", ".kts", 0, cl);
        ok(n >= 2 && file_exists("withsuffix.kts") && !file_exists("withsuffix.ktx") &&
           file_exists("dup.ktx"),
           "suffix change renames, and leaves the clashing file as it is");
        strbuf_free(cl);
    }
    kitty_set_session_suffix("");
}

/* ------------------------------------------------------------------ */
/* Planning: Arrange, folder moves and deletes, session moves, import names */

static bool plan_has(const struct ksp_plan *p, const char *from, const char *to)
{
    int i;
    for (i = 0; i < p->n; i++)
        if (!strcmp(p->from[i], from) && !strcmp(p->to[i], to))
            return true;
    return false;
}
static bool plan_has_folder(const struct ksp_plan *p, const char *name,
                            const char *value)
{
    int i;
    for (i = 0; i < p->nfolder; i++)
        if (!strcmp(p->fname[i], name) && !strcmp(p->fvalue[i], value))
            return true;
    return false;
}
static bool plan_has_clash(const struct ksp_plan *p, const char *name)
{
    int i;
    for (i = 0; i < p->nclash; i++)
        if (!stricmp(p->clash[i], name))
            return true;
    return false;
}

static void part_plan(void)
{
    struct ksp_plan p;

    head("Arrange: which sessions, where to, and clashes");
    {
        char *names[] = { "Default Settings", "srv01", "srv02", "root1",
                          "Linux\\web\\srv03", "db", "Linux\\db", "x" };
        char *folders[] = { "Linux", "Linux\\web", "BSD", "Default",
                            "ignored", "Linux", "Linux", "" };
        ksp_plan_arrange(names, folders, 8, &p);
        ok(p.n == 2, "two of the three bare sessions with a non-root Folder value move");
        ok(plan_has(&p, "srv01", "Linux\\web\\srv01"), "srv01 -> Linux\\web\\srv01");
        ok(plan_has(&p, "srv02", "BSD\\srv02"), "srv02 -> BSD\\srv02");
        ok(!plan_has(&p, "db", "Linux\\db"),
           "db -> Linux\\db is not planned: the path is taken, db stays");
        ok(!plan_has(&p, "Default Settings", "Linux\\Default Settings"),
           "Default Settings is never arranged");
        ok(p.nclash == 1 && plan_has_clash(&p, "Linux\\db"),
           "Linux\\db exists already: the one clash");
        ok(ksp_plan_arrange_offer_due(&p),
           "a taken folder path: the first-start offer is due");
        ksp_plan_free(&p);
    }
    {
        char *names[] = { "a", "A\\b" };
        char *folders[] = { "x", "" };
        char *n2[] = { "b", "c" };
        char *f2[] = { "Same", "same" };
        ksp_plan_arrange(names, folders, 2, &p);
        ok(p.n == 1 && p.nclash == 0, "nothing in the way: no clash");
        ok(!ksp_plan_arrange_offer_due(&p),
           "Folder= session with a free path beside a path session: no offer");
        ksp_plan_free(&p);
        ksp_plan_arrange(n2, f2, 2, &p);
        ok(p.n == 2 && p.nclash == 0, "two sessions into one folder, different names");
        ok(!ksp_plan_arrange_offer_due(&p),
           "Folder= sessions only, no clash: no offer");
        ksp_plan_free(&p);
    }
    head("Arrange: the free ones move, the taken ones stay; the offer only on a clash");
    {
        char *names[] = { "beta", "gamma", "work\\beta" };
        char *folders[] = { "work", "work", "" };
        char *n2[] = { "Default Settings", "root1", "Linux\\srv" };
        char *f2[] = { "Linux", "Default", "" };
        ksp_plan_arrange(names, folders, 3, &p);
        ok(p.n == 1 && p.nclash == 1 && plan_has_clash(&p, "work\\beta"),
           "beta (Folder=work) meets work\\beta: one clash, two to arrange");
        ok(plan_has(&p, "gamma", "work\\gamma") && !plan_has(&p, "beta", "work\\beta"),
           "the free gamma is planned, the clashing beta is not");
        ok(ksp_plan_arrange_offer_due(&p), "clash: the offer is due");
        ksp_plan_free(&p);
        {
            /* Every candidate clashes: nothing planned, the offer still due. */
            char *n3[] = { "beta", "work\\beta" };
            char *f3[] = { "work", "" };
            ksp_plan_arrange(n3, f3, 2, &p);
            ok(p.n == 0 && p.nclash == 1 && ksp_plan_arrange_offer_due(&p),
               "only a clashing session: nothing planned, one clash, offer due");
            ksp_plan_free(&p);
        }
        {
            /* Mixed: free ones planned, taken ones listed, nothing twice. */
            char *n4[] = { "a", "b", "c", "d", "x\\b", "y\\d", "Default Settings" };
            char *f4[] = { "x", "x", "y", "y", "", "", "x" };
            int i, j;
            bool twice = false, both = false;
            ksp_plan_arrange(n4, f4, 7, &p);
            ok(p.n == 2 && plan_has(&p, "a", "x\\a") && plan_has(&p, "c", "y\\c"),
               "mixed plan: the two free ones are planned");
            ok(p.nclash == 2 && plan_has_clash(&p, "x\\b") && plan_has_clash(&p, "y\\d"),
               "mixed plan: the two taken paths are listed");
            for (i = 0; i < p.n; i++) {
                for (j = i + 1; j < p.n; j++)
                    if (!stricmp(p.from[i], p.from[j]) || !stricmp(p.to[i], p.to[j]))
                        twice = true;
                for (j = 0; j < p.nclash; j++)
                    if (!stricmp(p.to[i], p.clash[j]))
                        both = true;
            }
            ok(!twice, "mixed plan: no session and no target planned twice");
            ok(!both, "mixed plan: no target both planned and listed as taken");
            ksp_plan_free(&p);
        }
        ksp_plan_arrange(names, folders, 2, &p);
        ok(p.n == 2 && !ksp_plan_arrange_offer_due(&p),
           "the same Folder= sessions without work\\beta: no offer");
        ksp_plan_free(&p);
        ksp_plan_arrange(n2, f2, 3, &p);
        ok(p.n == 0 && !ksp_plan_arrange_offer_due(&p),
           "root and path sessions only (Default Settings aside): no offer");
        ksp_plan_free(&p);
        ksp_plan_arrange(NULL, NULL, 0, &p);
        ok(!ksp_plan_arrange_offer_due(&p), "an empty store: no offer");
        ksp_plan_free(&p);
    }

    head("folder delete / rename: subfolders keep their shape");
    {
        char *names[] = { "net\\a", "net\\sub\\b", "net\\sub\\deep\\c", "other\\d",
                          "bare", "bare2", "a" };
        char *folders[] = { "", "", "", "", "net\\sub", "network", "" };
        ksp_plan_folder_move(names, folders, 7, "net", "Default", &p);
        ok(plan_has(&p, "net\\a", "a"), "net\\a -> a (to the root)");
        ok(plan_has(&p, "net\\sub\\b", "sub\\b"), "net\\sub\\b -> sub\\b");
        ok(plan_has(&p, "net\\sub\\deep\\c", "sub\\deep\\c"), "deep paths keep their shape");
        ok(!plan_has(&p, "other\\d", "d"), "another folder is not touched");
        ok(plan_has_folder(&p, "bare", "sub"), "a bare session filed below gets Folder=sub");
        ok(p.nfolder == 1, "\"network\" is not within \"net\"");
        ok(p.nclash == 1 && plan_has_clash(&p, "a"), "root session \"a\" clashes");
        ksp_plan_free(&p);

        ksp_plan_folder_move(names, folders, 7, "net", "keep", &p);
        ok(plan_has(&p, "net\\sub\\b", "keep\\sub\\b"), "to another folder: keep\\sub\\b");
        ok(plan_has_folder(&p, "bare", "keep\\sub"), "bare: Folder=keep\\sub");
        ok(p.nclash == 0, "no clash in an empty destination");
        ksp_plan_free(&p);

        ksp_plan_folder_move(names, folders, 7, "net\\sub", "NET", &p);
        ok(plan_has(&p, "net\\sub\\b", "NET\\b"), "a subfolder up one level");
        ok(p.nclash == 0, "\"NET\\b\" is free");
        ksp_plan_free(&p);
    }

    head("moving selected sessions into a folder");
    {
        char *names[] = { "x\\one", "two", "y\\one", "y\\two" };
        char *sel[] = { "x\\one", "two" };
        ksp_plan_sessions_move(names, 4, sel, 2, "y", &p);
        ok(plan_has(&p, "x\\one", "y\\one") && plan_has(&p, "two", "y\\two"),
           "both go to y under their own names");
        ok(p.nclash == 2, "and both names are taken there");
        ksp_plan_free(&p);
        ksp_plan_sessions_move(names, 4, sel, 2, "Default", &p);
        ok(plan_has(&p, "x\\one", "one") && plan_has(&p, "two", "two"),
           "to the root: leaf names");
        ok(p.nclash == 0, "no clash at the root");
        ksp_plan_free(&p);
        {
            char *s2[] = { "x\\one", "y\\one" };
            ksp_plan_sessions_move(names, 4, s2, 2, "z", &p);
            ok(p.nclash == 1 && plan_has_clash(&p, "z\\one"),
               "two moves to the same name clash with each other");
            ksp_plan_free(&p);
        }
    }

    head("import names: the old store's file -> identity");
    {
        char *s;
        s = ksp_import_name("Linux\\web\\srv01.ktx", ".ktx");
        ok_eq_str(s, "Linux\\web\\srv01", "suffix taken off the file name");
        sfree(s);
        s = ksp_import_name("a%3Ab\\%43ON", "");
        ok_eq_str(s, "a:b\\CON", "components unescaped");
        sfree(s);
        s = ksp_import_name("dir.ktx\\x.ktx", ".ktx");
        ok_eq_str(s, "dir.ktx\\x", "a directory keeps its ending");
        sfree(s);
        s = ksp_import_name("plain", ".ktx");
        ok_eq_str(s, "plain", "a file without the suffix keeps its name");
        sfree(s);
        s = ksp_import_name(".ktx", ".ktx");
        ok_eq_str(s, ".ktx", "nothing left in front: not stripped");
        sfree(s);
    }
}

/* ------------------------------------------------------------------ */
/* The registry move, in a private hive file (RegLoadAppKey): never the
 * user's registry. */

typedef LONG (WINAPI *RegLoadAppKeyA_t)(LPCSTR, PHKEY, REGSAM, DWORD, DWORD);

static void reg_put(HKEY root, const char *key, const char *val,
                    const void *data, DWORD len, DWORD type)
{
    HKEY k;
    if (RegCreateKeyExA(root, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k,
                        NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, val, 0, type, (const BYTE *)data, len);
        RegCloseKey(k);
    }
}
static bool reg_get(HKEY root, const char *key, const char *val, void *buf,
                    DWORD *len)
{
    HKEY k;
    LONG r;
    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    r = RegQueryValueExA(k, val, NULL, NULL, (BYTE *)buf, len);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
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
    char hive[1200], cmd[1300];
    HKEY root;
    int i;
    enum { BIG = 70000 };
    char *big = snewn(BIG, char), *back = snewn(BIG + 16, char);

    head("registry: a session key moves whole (private hive)");
    if (!load) {
        printf("  SKIP  RegLoadAppKey not available\n");
        sfree(big); sfree(back);
        return;
    }
    snprintf(hive, sizeof(hive), "%s\\kitty_test_sessionpath.hiv", tmp);
    snprintf(cmd, sizeof(cmd), "del /q \"%s*\" 2>nul", hive);
    system(cmd);
    if (load(hive, &root, KEY_ALL_ACCESS, 0, 0) != ERROR_SUCCESS) {
        ok(false, "a private hive file can be loaded");
        sfree(big); sfree(back);
        return;
    }
    for (i = 0; i < BIG - 1; i++)
        big[i] = (char)('a' + i % 26);
    big[BIG - 1] = '\0';
    reg_put(root, "S\\srv01", "HostName", "h.example", 10, REG_SZ);
    reg_put(root, "S\\srv01", "Commands", big, BIG, REG_SZ);     /* > 1024 bytes */
    reg_put(root, "S\\srv01", "Blob", big, 5000, REG_BINARY);
    reg_put(root, "S\\srv01\\Sub\\Deeper", "X", "y", 2, REG_SZ);

    ok(ksp_reg_move_tree(root, "S\\srv01", "S\\Linux%5Csrv01", NULL),
       "the move reports success");
    {
        DWORD len = BIG + 16;
        ok(reg_get(root, "S\\Linux%5Csrv01", "Commands", back, &len) &&
           len == BIG && !memcmp(back, big, BIG),
           "a 70000-byte value arrives whole");
        len = BIG + 16;
        ok(reg_get(root, "S\\Linux%5Csrv01", "Blob", back, &len) &&
           len == 5000 && !memcmp(back, big, 5000), "a binary value arrives whole");
        len = 16;
        ok(reg_get(root, "S\\Linux%5Csrv01\\Sub\\Deeper", "X", back, &len),
           "subkeys arrive, all the way down");
    }
    ok(!reg_exists(root, "S\\srv01"), "the source is gone after a proven copy");

    ok(ksp_reg_move_tree(root, "S\\Linux%5Csrv01", "S\\LINUX%5Csrv01", NULL),
       "a change of case only");
    {
        DWORD len = BIG + 16;
        char name[64];
        DWORD nl = sizeof(name);
        HKEY k;
        bool upper = false;
        ok(reg_get(root, "S\\LINUX%5Csrv01", "Commands", back, &len) && len == BIG,
           "the long value survives the case change");
        ok(!reg_exists(root, "S\\Linux%5Csrv01%rekey") &&
           !reg_exists(root, "S\\LINUX%5Csrv01%rekey"),
           "no temporary key is left behind");
        if (RegOpenKeyExA(root, "S", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegEnumKeyExA(k, 0, name, &nl, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
                upper = !strcmp(name, "LINUX%5Csrv01");
            RegCloseKey(k);
        }
        ok(upper, "the key now carries the new case");
    }

    /* The temporary key of a change of case is no session's key: the
     * session `srv02~rekey` (escaped srv02%7Erekey) survives the change of
     * case of `srv02`, while a leftover temporary key is cleared. */
    reg_put(root, "S\\srv02", "HostName", "a", 2, REG_SZ);
    reg_put(root, "S\\srv02%7Erekey", "HostName", "b", 2, REG_SZ);
    reg_put(root, "S\\srv02%rekey", "Stale", "x", 2, REG_SZ);
    ok(ksp_reg_move_tree(root, "S\\srv02", "S\\SRV02", NULL),
       "a change of case beside a session named ~rekey");
    {
        char v[8];
        DWORD len = sizeof(v);
        ok(reg_get(root, "S\\srv02%7Erekey", "HostName", v, &len) &&
           !strcmp(v, "b"), "the session srv02~rekey is untouched");
        len = sizeof(v);
        ok(reg_get(root, "S\\SRV02", "HostName", v, &len) && !strcmp(v, "a"),
           "the renamed session keeps its settings");
        len = sizeof(v);
        ok(!reg_get(root, "S\\SRV02", "Stale", v, &len),
           "nothing of the old temporary key gets into it");
        ok(!reg_exists(root, "S\\srv02%rekey"), "the stale temporary key is gone");
    }

    reg_put(root, "S\\taken", "HostName", "t", 2, REG_SZ);
    ok(!ksp_reg_move_tree(root, "S\\LINUX%5Csrv01", "S\\taken", NULL),
       "never onto a key that exists");
    ok(reg_exists(root, "S\\LINUX%5Csrv01"), "and the source is untouched then");
    ok(!ksp_reg_copy_verified(root, "S\\missing", "S\\new") &&
       !reg_exists(root, "S\\new"), "a missing source copies nothing");

    RegCloseKey(root);
    system(cmd);
    sfree(big);
    sfree(back);
}

/* ------------------------------------------------------------------ */
/* The folder store: a folder and a session sharing one name on disk,
 * a refused move moving nothing, and the listing's cost. */

static void part_store_blockers(void)
{
    head("a session file and a folder of the same name (no suffix)");
    kitty_set_session_suffix("");
    make_session("Box", "root-box.example", NULL);       /* the FILE Box */
    {
        char *errmsg = NULL;
        settings_w *w = open_settings_w("Box\\srv09", &errmsg);
        ok(w == NULL && errmsg && strstr(errmsg, "Box"),
           "saving Box\\srv09 is refused: the file Box is in the way");
        if (w)
            close_settings_w(w);
        sfree(errmsg);
    }
    {
        char *b = ksf_folder_blocker("Box\\deeper");
        ok_eq_str(b, "Box", "a new folder below Box names the file in the way");
        sfree(b);
    }
    write_file("Folder1\\s1", "HostName=s1.example\n");
    {
        char *errmsg = NULL;
        settings_w *w = open_settings_w("Folder1", &errmsg);
        ok(w == NULL && errmsg && strstr(errmsg, "Folder1"),
           "saving a session named like an existing folder is refused");
        if (w)
            close_settings_w(w);
        sfree(errmsg);
    }
    ok(!kitty_own_session_exists("Folder1"), "a directory is never a session");

    head("a refused move moves nothing");
    make_session("mv\\a", "a.example", NULL);
    make_session("mv\\b", "b.example", NULL);
    {
        char *from[] = { "mv\\a", "mv\\b" };
        char *to[] = { "dest\\a", "Box\\x\\b" };   /* the second is blocked */
        char *err = NULL;
        int r = kitty_session_rekey_many(from, to, 2, NULL, NULL, &err);
        ok(r == KITTY_REKEY_FAILED && err && strstr(err, "Nothing was moved"),
           "refused before the first move, saying so");
        ok(file_exists("mv\\a") && file_exists("mv\\b") && !file_exists("dest"),
           "both sessions are where they were");
        sfree(err);
    }
    {
        char *from[] = { "mv\\a", "mv\\b" };
        char *to[] = { "dest\\a", "dest\\b" };
        strbuf *rw = strbuf_new(), *cl = strbuf_new();
        char *err = NULL;
        int r = kitty_session_rekey_many(from, to, 2, rw, cl, &err);
        ok(r == KITTY_REKEY_OK && file_exists("dest\\a") && file_exists("dest\\b") &&
           !file_exists("mv"), "a clean batch moves, and the emptied folder goes");
        sfree(err);
        strbuf_free(rw);
        strbuf_free(cl);
    }
    del_settings("Box");
    del_settings("dest\\a");
    del_settings("dest\\b");
}

/* Set a file's last-write time to `seconds_ago` before now: the verdict
 * cache judges again any file written in the last few seconds, so a fixture
 * that is to be cached is aged first. */
static void set_age_full(const char *full, int seconds_ago)
{
    HANDLE h = CreateFileA(full, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    FILETIME ft;
    ULARGE_INTEGER u;
    if (h == INVALID_HANDLE_VALUE)
        return;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)seconds_ago * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

static void set_age(const char *rel, int seconds_ago)
{
    char *p = dupprintf("%s\\%s", g_dir, rel);
    set_age_full(p, seconds_ago);
    sfree(p);
}

/* A listing, and how many files it read. */
static unsigned long list_reads(int *n)
{
    unsigned long r0 = ksp_verdict_reads();
    char **names = kitty_session_names(n);
    unsigned long r = ksp_verdict_reads() - r0;
    kitty_session_names_free(names, *n);
    return r;
}

static void part_verdict_cache(const char *tmp)
{
    char save_dir[1024], cmd[1300];
    int n;
    unsigned long r;

    head("verdict cache: a listing reads only new and changed files");
    strcpy(save_dir, g_dir);
    snprintf(g_dir, sizeof(g_dir), "%s\\kitty_test_sessionpath_cache", tmp);
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);
    CreateDirectoryA(g_dir, NULL);
    kitty_set_session_dir(g_dir);
    kitty_set_session_suffix(".ktx");

    write_file("s1.ktx", "HostName=s1.example\n");
    write_file("f\\s2.ktx", "HostName=s2.example\n");
    write_file("f\\s3.ktx", "Protocol=ssh\n");
    write_file("note.ktx", "XostName=a\n");          /* same size as below */
    write_file("README.md", "# sessions\n");
    set_age("s1.ktx", 3600);
    set_age("f\\s2.ktx", 3600);
    set_age("f\\s3.ktx", 3600);
    set_age("note.ktx", 3600);
    set_age("README.md", 3600);

    r = list_reads(&n);
    ok(n == 3 && r == 5, "first listing: 3 sessions, all 5 files read");
    r = list_reads(&n);
    ok(n == 3 && r == 0, "second listing: nothing read again");

    write_file("s1.ktx", "HostName=s1.example\nPort=2222\n");
    set_age("s1.ktx", 1800);
    r = list_reads(&n);
    ok(n == 3 && r == 1, "a file changed in size and time is read again, alone");

    write_file("f\\s3.ktx", "Keep=0\n");             /* a session no more */
    set_age("f\\s3.ktx", 1800);
    r = list_reads(&n);
    ok(n == 2 && r == 1 && !listed("f\\s3"),
       "its new verdict is used: a session turned into a note goes");

    write_file("note.ktx", "HostName=a\n");
    set_age("note.ktx", 1200);                       /* same size, newer time */
    r = list_reads(&n);
    ok(n == 3 && r == 1 && listed("note"),
       "a note turned into a session (same size, newer time) appears");

    {
        int before = ksp_verdict_count();
        char *p = dupprintf("%s\\f\\s2.ktx", g_dir);
        DeleteFileA(p);
        sfree(p);
        r = list_reads(&n);
        ok(n == 2 && r == 0 && !listed("f\\s2"), "a deleted file disappears");
        ok(ksp_verdict_count() == before - 1, "and its verdict is dropped");
    }

    write_file("fresh.ktx", "HostName=fresh.example\n");   /* written just now */
    r = list_reads(&n);
    ok(n == 3 && r == 1, "a new file is read");
    r = list_reads(&n);
    ok(r == 1, "a file written in the last seconds is read at every listing");

    /* A save of this process that keeps size and time (one timestamp tick)
     * must not keep the old verdict: ksf_save forgets its file. */
    make_session("tick", "t.example", NULL);
    {
        char *p = dupprintf("%s\\tick.ktx", g_dir);
        FILE *fp = fopen(p, "rb");
        char buf[4096];
        size_t len = fp ? fread(buf, 1, sizeof(buf) - 1, fp) : 0;
        FILETIME old;
        HANDLE h;
        if (fp)
            fclose(fp);
        buf[len] = '\0';
        /* the same bytes with the key spoiled: a note of the same size */
        {
            char *k = strstr(buf, "HostName=");
            if (k)
                *k = 'X';
        }
        fp = fopen(p, "wb");
        if (fp) {
            fwrite(buf, 1, len, fp);
            fclose(fp);
        }
        set_age_full(p, 600);
        h = CreateFileA(p, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        GetFileTime(h, NULL, NULL, &old);
        CloseHandle(h);
        ok(!listed("tick"), "the spoiled file is no session (verdict cached)");
        make_session("tick", "t.example", NULL);      /* same size again */
        h = CreateFileA(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        SetFileTime(h, NULL, NULL, &old);             /* and the same time */
        CloseHandle(h);
        ok(listed("tick"), "a save with the same size and time is judged afresh");
        sfree(p);
    }

    kitty_set_session_suffix("");
    system(cmd);
    strcpy(g_dir, save_dir);
    kitty_set_session_dir(g_dir);
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* 500 session files of about 15 KB each - the size of a real KiTTY++
 * session - in 10 folders, the way #55's reporter keeps them. */
static void perf_fixture(const char *dir, const char *suffix, bool host_last)
{
    char *pad = snewn(16000, char);
    int i, j;
    size_t at = 0;
    for (j = 0; j < 300; j++)
        at += snprintf(pad + at, 16000 - at, "Setting%03d=%040d\n", j, j);
    for (i = 0; i < 500; i++) {
        char *p = dupprintf("%s\\f%02d\\srv%03d%s", dir, i % 10, i, suffix);
        FILE *fp;
        ksf_make_parent_dirs(p);
        fp = fopen(p, "wb");
        if (fp) {
            if (!host_last)
                fprintf(fp, "Present=1\nHostName=srv%03d.example\n", i);
            fputs(pad, fp);
            if (host_last)
                fprintf(fp, "HostName=srv%03d.example\n", i);
            fclose(fp);
            set_age_full(p, 3600);   /* settled: the cache may keep it */
        }
        sfree(p);
    }
    sfree(pad);
}

/* The same listing again, served from the verdict cache. */
static void perf_cached(const char *what)
{
    unsigned long r0 = ksp_verdict_reads();
    double t0 = now_ms(), t1;
    int n;
    char **names = kitty_session_names(&n);
    t1 = now_ms();
    printf("  TIME  %s, cached: %d sessions in %.1f ms, %lu files read\n",
           what, n, t1 - t0, ksp_verdict_reads() - r0);
    ok(n == 500 && ksp_verdict_reads() == r0,
       "the cached listing reads no file and lists all 500");
    kitty_session_names_free(names, n);
}

static void part_perf(const char *tmp)
{
    char dir[1100], cmd[1300];
    int n;
    double t0, t1;
    char **names;

    head("listing 500 session files of 15 KB (a measurement, not a verdict)");
    snprintf(dir, sizeof(dir), "%s\\kitty_test_sessionpath_perf", tmp);
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
    system(cmd);
    kitty_set_session_dir(dir);
    CreateDirectoryA(dir, NULL);

    kitty_set_session_suffix("");
    perf_fixture(dir, "", false);
    t0 = now_ms();
    names = kitty_session_names(&n);
    t1 = now_ms();
    printf("  TIME  no suffix, HostName in line 2:   %d sessions in %.1f ms\n", n, t1 - t0);
    ok(n == 500, "all 500 listed");
    kitty_session_names_free(names, n);
    perf_cached("no suffix, HostName in line 2");

    t0 = now_ms();
    {
        char *res = NULL, *err = NULL;
        int k = kitty_session_resolve("srv250", &res, &err, 1);
        t1 = now_ms();
        printf("  TIME  bare-name lookup srv250:      %.1f ms\n", t1 - t0);
        ok(k == KSP_UNIQUE && res && !strcmp(res, "f00\\srv250"), "srv250 found in f00");
        sfree(res); sfree(err);
    }
    t0 = now_ms();
    {
        int k = kitty_session_resolve("no.such.host", NULL, NULL, 1);
        t1 = now_ms();
        printf("  TIME  a hostname that is no session: %.1f ms\n", t1 - t0);
        ok(k == KSP_NONE, "a host name that is no session resolves to nothing");
    }

    system(cmd);
    CreateDirectoryA(dir, NULL);
    perf_fixture(dir, "", true);
    t0 = now_ms();
    names = kitty_session_names(&n);
    t1 = now_ms();
    printf("  TIME  no suffix, HostName last line: %d sessions in %.1f ms\n", n, t1 - t0);
    ok(n == 500, "all 500 listed when HostName comes last");
    kitty_session_names_free(names, n);
    perf_cached("no suffix, HostName last line");

    system(cmd);
    CreateDirectoryA(dir, NULL);
    kitty_set_session_suffix(".ktx");
    perf_fixture(dir, ".ktx", true);
    t0 = now_ms();
    names = kitty_session_names(&n);
    t1 = now_ms();
    printf("  TIME  suffix .ktx, HostName last:    %d sessions in %.1f ms\n", n, t1 - t0);
    ok(n == 500, "all 500 suffixed files listed by their content");
    kitty_session_names_free(names, n);
    perf_cached("suffix .ktx, HostName last");
    kitty_set_session_suffix("");

    system(cmd);
    kitty_set_session_dir(g_dir);
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TEMP");
    char cmd[1200];
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(g_dir, sizeof(g_dir), "%s\\kitty_test_sessionpath", tmp);

    part_pure();
    part_plan();
    part_registry(tmp);

    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", g_dir);
    system(cmd);
    kitty_set_storage_mode(1);
    kitty_set_session_dir(g_dir);
    if (store_is_file()) {
        part_store();
        part_store_blockers();
        part_verdict_cache(tmp);
        part_perf(tmp);
    } else
        ok(false, "storage is in file mode against the fixture");
    kitty_set_storage_mode(0);
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
