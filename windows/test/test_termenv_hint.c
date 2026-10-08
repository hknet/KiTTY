/*
 * test_termenv_hint - the "this host refuses COLORTERM / TERM_PROGRAM" mark
 * (kitty/kitty_termenv_hint.c) through the real folder store: a private
 * %TEMP% tree set up as the store in use (savemode=dir), removed afterwards.
 * The registry half is the same rule on RegQueryValueEx / RegSetValueEx and
 * is not run here: this test never writes the registry.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "putty.h"   /* pulls in windows.h (winsock2-first) via platform.h */
#include "../kitty/kitty_storage.h"
#include "../kitty/kitty_termenv.h"

/* the sinks the linked store code expects (console-app style) */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "nonfatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static int file_holds(const char *path, const char *want)
{
    char buf[64];
    size_t n;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return !strcmp(buf, want);
}

int main(void)
{
    char tmp[MAX_PATH], root[MAX_PATH], sess[MAX_PATH], hint[MAX_PATH];
    char f1[MAX_PATH], f2[MAX_PATH];
    char id[KITTY_TERMENV_HOST_ID_MAX];

    GetTempPathA(sizeof(tmp), tmp);
    snprintf(root, sizeof(root), "%skitty-termenv-hint-%lu",
             tmp, (unsigned long)GetCurrentProcessId());
    snprintf(sess, sizeof(sess), "%s\\Sessions", root);
    snprintf(hint, sizeof(hint), "%s\\TermEnvHint", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(sess, NULL);

    kitty_set_session_dir(sess);
    kitty_set_storage_mode(1);
    check(store_is_file(), "the temp tree is the store in use");

    kitty_termenv_host_id("QA.Example", 2222, id, sizeof(id));
    check(!strcmp(id, "qa.example:2222"), "host id qa.example:2222");
    check(kitty_termenv_hint_first(id), "first refusal: the NOTE is due");
    snprintf(f1, sizeof(f1), "%s\\qa.example%%3A2222", hint);
    check(file_holds(f1, "1\n"),
          "the mark is TermEnvHint\\qa.example%3A2222 holding 1");
    check(!kitty_termenv_hint_first(id), "second refusal: no NOTE");
    check(!kitty_termenv_hint_first(id), "third refusal: no NOTE");

    kitty_termenv_host_id("qa.example", 22, id, sizeof(id));
    check(kitty_termenv_hint_first(id), "the same name on port 22 is another host");
    snprintf(f2, sizeof(f2), "%s\\qa.example", hint);
    check(file_holds(f2, "1\n"), "its mark is TermEnvHint\\qa.example");
    check(!kitty_termenv_hint_first(id), "and is then known");

    DeleteFileA(f1);
    check(kitty_termenv_hint_first("qa.example:2222"),
          "a removed mark: the NOTE is due again");

    DeleteFileA(f1);
    DeleteFileA(f2);
    RemoveDirectoryA(hint);
    RemoveDirectoryA(sess);
    RemoveDirectoryA(root);
    check(GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES,
          "the temp tree is gone");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
