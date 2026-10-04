/*
 * test_sessmenu.c - kageant's Saved Sessions menu as the folder tree
 * (hknet/KiTTY#55): kitty/kitty_sessmenu.c.
 *
 * Part one drives ksm_build with lists of identities and Folder values: the
 * nesting, the path-wins rule, folders first, the number-aware order by the
 * shown name, folders that differ only in case, Default Settings left out.
 * Part two reads Folder values in both on-disk line forms. Part three walks a
 * folder store the test creates under %TEMP% and removes (nested folders, a
 * suffix, a dot-file, a non-session README, an unarranged session filed by
 * its Folder value) and builds the menu from it, as kageant does - including
 * the Folder cache, which reads a file again only when it changed.
 *
 * Linked like kageant: kitty_sessmenu.c + kitty_sessionpath.c + utils, NO
 * settings library - so this test failing to link is kageant failing to link.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty/kitty_sessionpath.h"
#include "kitty/kitty_sessmenu.h"

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

/* The menu as one line: "[Linux [web srv01 ] k8 ] top" - a folder opens with
 * '[' + name, closes with ']'; a session is its label (and with ids, "=id"). */
static char *menu_text(char *const *ids, char *const *folders, int n,
                       bool with_ids)
{
    int ne, i;
    struct ksm_entry *e = ksm_build(ids, folders, n, &ne);
    strbuf *sb = strbuf_new();
    for (i = 0; i < ne; i++) {
        if (sb->len)
            put_byte(sb, ' ');
        if (e[i].kind == KSM_FOLDER_OPEN) {
            put_byte(sb, '[');
            put_dataz(sb, e[i].label);
        } else if (e[i].kind == KSM_FOLDER_CLOSE) {
            put_byte(sb, ']');
        } else {
            put_dataz(sb, e[i].label);
            if (with_ids) {
                put_byte(sb, '=');
                put_dataz(sb, ids[e[i].idx]);
            }
        }
    }
    ksm_free(e, ne);
    return strbuf_to_str(sb);
}

static void expect_menu(char *const *ids, char *const *folders, int n,
                        bool with_ids, const char *want, const char *what)
{
    char *got = menu_text(ids, folders, n, with_ids);
    bool same = !strcmp(got, want);
    ok(same, what);
    if (!same)
        printf("      want: %s\n      got:  %s\n", want, got);
    sfree(got);
}

static void part_build(void)
{
    {
        char *ids[] = {
            "Linux\\web\\srv01", "Linux\\web\\srv10", "Linux\\web\\srv2",
            "Linux\\k78", "Linux\\k8", "top", "Default Settings",
            "unarranged", "Alpha",
        };
        char *folders[] = {
            /* a path's own Folder value never counts */
            "Elsewhere", NULL, NULL,
            NULL, NULL, "Default", "Linux",
            "Linux\\web", "",
        };
        expect_menu(ids, folders, lenof(ids), true,
                    "[Linux [web srv01=Linux\\web\\srv01 srv2=Linux\\web\\srv2 "
                    "srv10=Linux\\web\\srv10 unarranged=unarranged ] "
                    "k8=Linux\\k8 k78=Linux\\k78 ] Alpha=Alpha top=top",
                    "nested submenus, folders first, number-aware order, "
                    "the path wins, a bare name by its Folder, "
                    "Default Settings left out");
    }
    {
        /* every folder above a session is a submenu, even with no session
         * of its own; empty components are dropped */
        char *ids[] = { "a\\b\\c\\deep", "x" };
        char *folders[] = { NULL, "\\q\\\\r\\" };
        expect_menu(ids, folders, lenof(ids), false,
                    "[a [b [c deep ] ] ] [q [r x ] ]",
                    "implied folders nest; a Folder value is normalised");
    }
    {
        /* folders that differ only in case are one; it shows the spelling
         * that sorts first by byte, in any input order */
        char *ids1[] = { "linux\\b", "Linux\\a" };
        char *ids2[] = { "Linux\\a", "linux\\b" };
        expect_menu(ids1, NULL, 2, false, "[Linux a b ]",
                    "one folder for two spellings (order 1)");
        expect_menu(ids2, NULL, 2, false, "[Linux a b ]",
                    "one folder for two spellings (order 2)");
    }
    {
        /* folder names sort among themselves the same way */
        char *ids[] = { "net10\\a", "net9\\a", "Net-2\\a", "net\\a" };
        expect_menu(ids, NULL, lenof(ids), false,
                    "[net a ] [Net-2 a ] [net9 a ] [net10 a ]",
                    "folder submenus in number-aware, case-insensitive order");
    }
    {
        /* two sessions of one shown name in one folder (one by path, one by
         * Folder value): both listed, a total order */
        char *ids[] = { "web\\srv", "srv" };
        char *folders[] = { NULL, "web" };
        expect_menu(ids, folders, 2, true, "[web srv=srv srv=web\\srv ]",
                    "same shown name twice: both, in a fixed order");
    }
    {
        char *ids[] = { "Default Settings" };
        int ne = -1;
        struct ksm_entry *e = ksm_build(ids, NULL, 1, &ne);
        ok(ne == 0, "only Default Settings: an empty menu");
        ksm_free(e, ne);
    }
}

static void expect_folder(const char *text, const char *want, const char *what)
{
    char *got = ksm_text_folder(text, strlen(text));
    bool same = want ? (got && !strcmp(got, want)) : !got;
    ok(same, what);
    if (!same)
        printf("      want: %s\n      got:  %s\n", want ? want : "(none)",
               got ? got : "(none)");
    sfree(got);
}

static void part_folder_value(void)
{
    expect_folder("HostName=h\r\nFolder=Linux%5Cweb\r\nProtocol=ssh\r\n",
                  "Linux\\web", "KiTTY++ form, escaped '\\'");
    expect_folder("HostName\\h\\\nFolder\\Linux%5Cweb\\\n",
                  "Linux\\web", "classic KiTTY form");
    expect_folder("HostName=h\nFolderX=nope\nFolders=no\n", NULL,
                  "a key that only starts with Folder is not it");
    expect_folder("HostName=h\n", NULL, "no Folder line");
    expect_folder("Folder=a\nFolder=b\n", "b", "the last Folder line wins");
    expect_folder("Folder=\n", "", "an empty Folder value");
}

static char g_dir[1024];

static void put_file(const char *rel, const char *text)
{
    char path[1400];
    FILE *fp;
    snprintf(path, sizeof(path), "%s\\%s", g_dir, rel);
    fp = fopen(path, "wb");
    if (fp) {
        fputs(text, fp);
        fclose(fp);
    }
}

/* Back-date a file by an hour (its size stays what it is). */
static void age_file(const char *rel)
{
    char path[1400];
    HANDLE h;
    FILETIME ft;
    ULARGE_INTEGER u;
    snprintf(path, sizeof(path), "%s\\%s", g_dir, rel);
    h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 3600ULL * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

static void mkdir_rel(const char *rel)
{
    char path[1400];
    snprintf(path, sizeof(path), "%s\\%s", g_dir, rel);
    CreateDirectoryA(path, NULL);
}

static void part_store(void)
{
    char **ids, **folders;
    int n;
    unsigned long reads;
    char *menu;

    mkdir_rel("Linux");
    mkdir_rel("Linux\\web");
    mkdir_rel(".git");
    put_file("Linux\\web\\srv01.ktx", "HostName=srv01\nProtocol=ssh\n");
    put_file("Linux\\web\\srv2.ktx", "HostName=srv2\n");
    put_file("Linux\\k8.ktx", "Protocol=ssh\n");
    put_file("unarranged.ktx", "HostName=u\nFolder=Linux%5Cweb\n");
    put_file("classic.ktx", "HostName\\c\\\nFolder\\Win\\\n");
    put_file("top.ktx", "HostName=t\n");
    put_file("Default%20Settings.ktx", "HostName=\nFolder=Linux\n");
    put_file(".gitignore", "HostName=ignored\n");          /* a dot-file */
    put_file(".git\\HEAD", "HostName=ignored\n");          /* a dot-folder */
    put_file("README.txt", "Sessions live here.\n");      /* no session */
    put_file("Linux\\notes.ktx", "just notes\n");          /* suffix, no session */

    /* An hour old, so the Folder cache may trust what it read (a file
     * written in the last few seconds is read at every build). */
    age_file("unarranged.ktx");
    age_file("classic.ktx");
    age_file("top.ktx");
    age_file("Default%20Settings.ktx");

    reads = ksm_folder_reads();
    n = ksm_folder_store_rows(g_dir, ".ktx", &ids, &folders);
    ok(ksm_folder_reads() - reads == 4,
       "first build: the Folder value of the 4 top-level bare-name files read");
    menu = menu_text(ids, folders, n, true);
    {
        const char *want =
            "[Linux [web srv01=Linux\\web\\srv01 srv2=Linux\\web\\srv2 "
            "unarranged=unarranged ] k8=Linux\\k8 ] "
            "[Win classic=classic ] top=top";
        bool same = !strcmp(menu, want);
        ok(same, "folder store: nested, sorted, unarranged by Folder, "
           "dot-files / README / non-session / Default Settings absent");
        if (!same)
            printf("      want: %s\n      got:  %s\n", want, menu);
    }
    sfree(menu);
    ksm_rows_free(ids, folders, n);

    /* The menu built again reads no file for its Folder value ... */
    reads = ksm_folder_reads();
    n = ksm_folder_store_rows(g_dir, ".ktx", &ids, &folders);
    ksm_rows_free(ids, folders, n);
    ok(ksm_folder_reads() == reads, "second build: no Folder value read again");

    /* ... until a file changes: that one is read, and its new Folder shows. */
    put_file("top.ktx", "HostName=t\nFolder=Moved\n");
    age_file("top.ktx");
    reads = ksm_folder_reads();
    n = ksm_folder_store_rows(g_dir, ".ktx", &ids, &folders);
    ok(ksm_folder_reads() - reads == 1, "a changed file: only it read again");
    menu = menu_text(ids, folders, n, false);
    ok(strstr(menu, "[Moved top ]") != NULL, "a changed Folder value moves the session");
    if (!strstr(menu, "[Moved top ]"))
        printf("      got:  %s\n", menu);
    sfree(menu);
    ksm_rows_free(ids, folders, n);
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TEMP");
    char cmd[1200];
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(g_dir, sizeof(g_dir), "%s\\kitty_test_sessmenu", tmp);

    part_build();
    part_folder_value();

    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);
    CreateDirectoryA(g_dir, NULL);
    part_store();
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", g_dir);
    system(cmd);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
