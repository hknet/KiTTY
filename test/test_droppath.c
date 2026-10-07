/*
 * test_droppath - regression tests for kitty/kitty_droppath.c: the remote
 * path of a dropped file in each target case, its POSIX shell quoting, and
 * the text typed after a drag-and-drop upload, plain and bracketed.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_droppath.h"

static int failures = 0;

static void check(const char *what, char *got, const char *want)
{
    if ((got == NULL) != (want == NULL) ||
        (got && strcmp(got, want) != 0)) {
        printf("FAIL [%s]: got <%s>, want <%s>\n", what,
               got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
    free(got);
}

static void remote(const char *dir, const char *local, const char *want)
{
    char what[256];
    snprintf(what, sizeof(what), "remote %s + %s", dir ? dir : "(null)", local);
    check(what, kitty_droppath_remote(dir, local), want);
}

static void quote(const char *path, const char *want)
{
    char what[256];
    snprintf(what, sizeof(what), "quote %s", path);
    check(what, kitty_droppath_quote(path), want);
}

int main(void)
{
    /* The target directory: OSC 7 (absolute), fixed, home. */
    remote("/home/u/src", "C:\\Users\\me\\shot.png", "/home/u/src/shot.png");
    remote("/home/u/src/", "C:\\Users\\me\\shot.png", "/home/u/src/shot.png");
    remote("/", "C:\\a.txt", "/a.txt");
    remote("/srv/in", "C:\\Users\\me\\project", "/srv/in/project");
    remote("/srv/in", "C:\\Users\\me\\project\\", "/srv/in/project");
    remote("~/drop", "C:\\a.txt", "~/drop/a.txt");
    remote("uploads", "C:\\a.txt", "~/uploads/a.txt");
    remote("./uploads", "C:\\a.txt", "~/uploads/a.txt");
    remote(".", "C:\\Users\\me\\shot.png", "~/shot.png");
    remote("", "C:\\Users\\me\\shot.png", "~/shot.png");
    remote(NULL, "shot.png", "~/shot.png");
    remote(".", "C:\\", NULL);
    remote(".", "", NULL);

    /* Quoting. */
    quote("/home/u/shot.png", "/home/u/shot.png");
    quote("/home/u/my shot.png", "'/home/u/my shot.png'");
    quote("/home/u/it's.png", "'/home/u/it'\\''s.png'");
    quote("/tmp/$HOME`x`;rm", "'/tmp/$HOME`x`;rm'");
    quote("~/shot.png", "~/shot.png");
    quote("~/my shot.png", "~/'my shot.png'");
    quote("~/it's", "~/'it'\\''s'");
    quote("~bob/a b", "~bob/'a b'");
    quote("~b$b/a", "'~b$b/a'");
    quote("", "''");

    /* What is typed. */
    {
        const char *one[] = { "/home/u/shot.png" };
        const char *several[] = { "/home/u/a.png", NULL, "~/b c.png", "/x/it's" };
        const char *none[] = { NULL, NULL };
        check("text one plain", kitty_droppath_text(one, 1, 0),
              "/home/u/shot.png");
        check("text one bracketed", kitty_droppath_text(one, 1, 1),
              "\033[200~/home/u/shot.png\033[201~");
        check("text several plain", kitty_droppath_text(several, 4, 0),
              "/home/u/a.png ~/'b c.png' '/x/it'\\''s'");
        check("text several bracketed", kitty_droppath_text(several, 4, 1),
              "\033[200~/home/u/a.png ~/'b c.png' '/x/it'\\''s'\033[201~");
        check("text none", kitty_droppath_text(none, 2, 0), NULL);
        check("text zero", kitty_droppath_text(one, 0, 1), NULL);
    }

    if (failures) {
        printf("test_droppath: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_droppath: all passed\n");
    return 0;
}
