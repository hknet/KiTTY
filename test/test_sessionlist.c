/*
 * test_sessionlist.c - the session list's BASELINE behaviour.
 *
 * Written before the folder-navigation work (design/TASK_session_folder_rows.md
 * in the private docs) so that changes to the list can be shown not to have
 * broken what it does today. It states the rules as assertions; if one of them
 * has to change, the change is deliberate and visible in the diff of this file.
 *
 * HERMETIC BY CONSTRUCTION: it never touches the registry or the user's
 * sessions. kitty_set_storage_mode(1) + kitty_set_session_dir(<temp>) puts the
 * whole storage layer into file mode against a directory this test creates and
 * deletes, which is the same path a portable install uses. That matters more
 * than usual here - the tests that came before this one lost a real person's
 * remembered keys by writing to live settings.
 *
 * Covers the MODEL only: enumeration, ordering, the folder attribute, and the
 * name round-trip. What the window does with those rows is layer B.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"

/* kitty/kitty_storage.c - public, but declared where their callers need them
 * rather than in a shared header. */
void kitty_set_storage_mode(int mode);
void kitty_set_session_dir(const char *dir);
int  store_is_file(void);
char *kitty_read_session_folder(const char *sessionname);

/*
 * Stubs for the platform edges the storage layer touches. test_conf carries the
 * same block; it needs fewer of them only because it never calls the storage
 * functions, so the linker never pulls storage.c in.
 */
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

/* The jump list is shell integration; a test has no business writing to it. */
void add_session_to_jumplist(const char * const sessionname) {}
void remove_session_from_jumplist(const char * const sessionname) {}
void clear_jumplist(void) {}

/* Pulled in through the network library, never reached from these paths. */
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
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void ok_eq_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (got && !strcmp(got, want)) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s (got \"%s\", wanted \"%s\")\n",
               what, got ? got : "(null)", want);
        failures++;
    }
}

static void head(const char *s) { printf("\n=== %s ===\n", s); }

/* Write one session the way KiTTY writes them, so the fixture is not a
 * hand-made approximation of the format. */
static void make_session(const char *name, const char *host, const char *folder)
{
    char *errmsg = NULL;
    settings_w *w = open_settings_w(name, &errmsg);
    if (!w) {
        printf("  FAIL  could not create fixture session \"%s\": %s\n",
               name, errmsg ? errmsg : "(no message)");
        failures++;
        return;
    }
    write_setting_s(w, "HostName", host);
    if (folder)
        write_setting_s(w, "Folder", folder);
    close_settings_w(w);
}

static const char *nth_session(struct sesslist *sl, int i)
{
    return (i >= 0 && i < sl->nsessions) ? sl->sessions[i] : "(out of range)";
}

static int index_of(struct sesslist *sl, const char *name)
{
    for (int i = 0; i < sl->nsessions; i++)
        if (!strcmp(sl->sessions[i], name))
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    char dir[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = ".";
    snprintf(dir, sizeof(dir), "%s\\kitty_test_sessionlist", tmp);

    /* A fresh directory every run: a leftover session from a previous run would
     * quietly change the expected order. */
    {
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "mkdir \"%s\" 2>nul", dir);
        system(cmd);
    }

    kitty_set_storage_mode(1);
    kitty_set_session_dir(dir);

    head("the fixture store is the one we are talking to");
    ok(store_is_file(), "storage is in file mode, not the registry");
    printf("  sessions in: %s\n", dir);

    head("enumeration");
    make_session("alpha", "a.example", NULL);
    make_session("beta", "b.example", "work");
    make_session("gamma", "c.example", "work");
    {
        struct sesslist sl;
        get_sesslist(&sl, true);
        ok(sl.nsessions == 4, "3 saved sessions + Default Settings are listed");
        ok(index_of(&sl, "alpha") > 0, "alpha is in the list");
        ok(index_of(&sl, "beta") > 0, "beta is in the list");
        ok(index_of(&sl, "gamma") > 0, "gamma is in the list");
        get_sesslist(&sl, false);
    }

    head("order");
    /* Default Settings first, then natural order: 2 before 10, which plain
     * strcmp gets wrong. Shipped 0.84.1.73-beta; this is the regression guard. */
    make_session("host2", "2.example", NULL);
    make_session("host10", "10.example", NULL);
    make_session("host1", "1.example", NULL);
    {
        struct sesslist sl;
        get_sesslist(&sl, true);
        ok_eq_str(nth_session(&sl, 0), "Default Settings",
                  "Default Settings comes first, always");
        int i1 = index_of(&sl, "host1");
        int i2 = index_of(&sl, "host2");
        int i10 = index_of(&sl, "host10");
        ok(i1 < i2 && i2 < i10, "host1 < host2 < host10 (natural, not ASCII)");
        get_sesslist(&sl, false);
    }

    head("the folder attribute");
    {
        settings_r *r = open_settings_r("beta");
        char *folder = r ? read_setting_s(r, "Folder") : NULL;
        ok_eq_str(folder, "work", "a session's Folder round-trips");
        sfree(folder);
        if (r) close_settings_r(r);

        r = open_settings_r("alpha");
        folder = r ? read_setting_s(r, "Folder") : NULL;
        ok(folder == NULL, "a session with no Folder reads back as none");
        sfree(folder);
        if (r) close_settings_r(r);
    }

    head("the folder a mid-session save binds to");
    /*
     * Mid-session (Change Settings) a save must not re-file the session, and it
     * reads the folder from STORAGE to decide what to write back rather than
     * from the running Conf - which was filled when the session launched and is
     * stale the moment the session is moved from another window. These are that
     * contract, at the model level; whether the dialog actually calls it is
     * layer B and is checked by hand.
     */
    make_session("filed", "filed.example", "work");
    {
        char *f = kitty_read_session_folder("filed");
        ok_eq_str(f, "work", "a filed session reports the folder it is in");
        sfree(f);

        /* The other window moves it while the terminal stays open. */
        make_session("filed", "filed.example", "network");
        f = kitty_read_session_folder("filed");
        ok_eq_str(f, "network",
                  "a move made elsewhere is what a later save reads back");
        sfree(f);

        f = kitty_read_session_folder("no-such-session");
        ok(f == NULL || !*f,
           "a name that does not exist yet reports no folder");
        sfree(f);

        f = kitty_read_session_folder("alpha");
        ok(f == NULL || !*f || !strcmp(f, "Default"),
           "an unfiled session reports no folder, not a stray one");
        sfree(f);
    }

    head("names that need escaping survive the round trip");
    /* The file store munges a session name into a file name; a name with a
     * space, a dot and punctuation is where that goes wrong if it goes wrong. */
    make_session("a b.c-d_e", "punct.example", "work");
    {
        struct sesslist sl;
        get_sesslist(&sl, true);
        ok(index_of(&sl, "a b.c-d_e") > 0,
           "\"a b.c-d_e\" comes back with its name intact");
        get_sesslist(&sl, false);

        settings_r *r = open_settings_r("a b.c-d_e");
        char *host = r ? read_setting_s(r, "HostName") : NULL;
        ok_eq_str(host, "punct.example", "and its settings are readable");
        sfree(host);
        if (r) close_settings_r(r);
    }

    head("deletion");
    del_settings("alpha");
    {
        struct sesslist sl;
        get_sesslist(&sl, true);
        ok(index_of(&sl, "alpha") < 0, "a deleted session leaves the list");
        ok(index_of(&sl, "beta") > 0, "and the others stay");
        get_sesslist(&sl, false);
    }

    head("Default Settings is claimed to exist even when it does not");
    {
        struct sesslist sl;
        get_sesslist(&sl, true);
        ok(index_of(&sl, "Default Settings") == 0,
           "it is listed, and first, with no file behind it");
        get_sesslist(&sl, false);
    }

    head("a folder stored on Default Settings is ignored");
    /* It is shown at every level, so a folder on it is invisible - and it is
     * inherited by everything created from the defaults, and it feeds the
     * folder-list rebuild. Written here the way an older version would have
     * left it behind. Last, because it puts a file behind Default Settings and
     * the section above asserts that there is none. */
    make_session("Default Settings", "d.example", "work");
    {
        char *f = kitty_read_session_folder("Default Settings");
        ok(f == NULL || !*f, "Default Settings reports no folder even with one stored");
        sfree(f);
    }

    {
        char cmd[1100];
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
        system(cmd);
    }

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
